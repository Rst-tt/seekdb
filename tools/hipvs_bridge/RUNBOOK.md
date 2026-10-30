# AMD 赛题 · GPU 节点执行手册（RUNBOOK）

目标：在 Radeon Cloud 的 `seekdb-hipvs` 工作区里，拿到 **端到端 CAGRA 构建耗时 + Recall@10/@100**，
用于选定最终提交参数（fp16 / graph_degree / intermediate_degree / niter），并证明 ≥3x 构建加速。

时间预算：单实例最长 4 小时、4 Credits/轮。**先跑最贵的编译，再并行做基准**。

---

## 0. 上传与落盘

本地已就绪（`C:\Users\zhaow\Documents\deepseek-harness\default-workspace\amd-bridge\`）：

| 文件 | 用途 |
| --- | --- |
| `seekdb_cuvs_bridge.c` | 优化版 bridge（FP16 + 参数开关 + 分阶段计时） |
| `bench_build.c` / `bench.sh` | 纯 bridge 的构建/召回基准（秒级迭代） |
| `probe_node.sh` | 节点一次性盘点 |
| `bench_e2e.py` | 端到端 DDL 计时（官方口径） |
| `start_seekdb_node.sh` | 后台起 seekdb 并等端口 |
| `stub/` + `stub_impl.c` | 本机 CPU 桩（不需要 GPU，已验证） |

上传（从 Windows 宿主机，WSL 内网络不通）：

```bash
wsl -d Ubuntu-22.04 -u zhaow -- bash -lc 'cp /mnt/c/Users/zhaow/Documents/deepseek-harness/default-workspace/amd-bridge/{seekdb_cuvs_bridge.c,bench_build.c,bench.sh,probe_node.sh,bench_e2e.py,start_seekdb_node.sh} /home/zhaow/amd-base/tools/hipvs_bridge/'
```

节点侧只需 `tools/hipvs_bridge/` 这几份文件；seekdb 源码用官方 `amd-cagra-submit`（= V5 冻结 commit `634888d9b`）。

---

## 1. 盘点（60 秒）

```bash
bash tools/hipvs_bridge/probe_node.sh 2>&1 | tee /workspace/probe.log
```

关注四项：

1. `/opt/hipvs/include/cuvs/core/c_api.h` 与 `/opt/hipvs/lib/libcuvs_c.so` 是否存在；
2. 是否已有编译好的 `seekdb`（`build_release/src/observer/seekdb` 或模板自带），有则**跳过编译**；
3. 是否有真实数据集（`*.fvecs`、`*sift*`、`*hidden*`）；
4. `python3 -c "import pymysql"`、`mysql`/`obclient` 客户端是否可用。

---

## 2. 编译 bridge（10 秒）

```bash
export HIPVS_ROOT=/opt/hipvs
export ROCM_ROOT=/opt/rocm
mkdir -p /workspace/bridge
HIPVS_ROOT=$HIPVS_ROOT ROCM_ROOT=$ROCM_ROOT \
  tools/hipvs_bridge/build.sh /workspace/bridge/libseekdb_cuvs_bridge.so 2>&1 | tail -20
```

---

## 3. 纯 bridge 基准（每次 1–3 分钟）

```bash
cd tools/hipvs_bridge
cp /workspace/bridge/libseekdb_cuvs_bridge.so ./libseekdb_cuvs_bridge.so
bash bench.sh quick 2>&1 | tee /workspace/bench_quick.log
```

`quick` = fp32 / fp16 各一轮，取 250K×128（SIFT 形状）与 250K×256（Hidden 形状）两组：

```bash
bash bench.sh one "SEEKDB_CUVS_TIME=1"                                   # fp32 默认
bash bench.sh one "SEEKDB_CUVS_FP16=1 SEEKDB_CUVS_TIME=1"                # fp16 默认
bash bench.sh one "SEEKDB_CUVS_FP16=1 SEEKDB_CUVS_INTERMEDIATE_DEGREE=128 SEEKDB_CUVS_TIME=1"
bash bench.sh one "SEEKDB_CUVS_FP16=1 SEEKDB_CUVS_GRAPH_DEGREE=96 SEEKDB_CUVS_INTERMEDIATE_DEGREE=128 SEEKDB_CUVS_NITER=60 SEEKDB_CUVS_TIME=1"
bash bench.sh one "SEEKDB_CUVS_FP16=1 SEEKDB_CUVS_GRAPH_DEGREE=64 SEEKDB_CUVS_INTERMEDIATE_DEGREE=96 SEEKDB_CUVS_NITER=40 SEEKDB_CUVS_TIME=1"
```

**读数规则**：`cagra_build_*` 是构图本体耗时，`build_total` 含 H2D；`recall@10/@100` 必须
SIFT ≥0.995（基线 1.000，门槛“较基线下降 ≤0.005”）、Hidden ≥0.90/0.85。

---

## 4. 端到端计时（官方口径，重要）

评测从 `CREATE VECTOR INDEX ... lib=cuvs` 开始计到第一条 `APPROXIMATE` 查询返回。先用官方口径量一次基线，
再量优化版，才有资格谈加速比（纯 bridge 数字只能做相对参考）。

```bash
# 后台起服务（SEEKDB_CUVS_TIME 打印 bridge 分阶段耗时，只用于诊断）
SEEKDB_CUVS_TIME=1 bash tools/hipvs_bridge/start_seekdb_node.sh \
  build_release/src/observer/seekdb 12899 /workspace/db_e2e

python3 tools/hipvs_bridge/bench_e2e.py --port 12899 --n 250000 --dim 128 --repeat 2 --tag baseline
python3 tools/hipvs_bridge/bench_e2e.py --port 12899 --n 250000 --dim 256 --repeat 2 --tag hidden-shape
# 若有真实数据：--base /workspace/sift/base.fvecs
```

对照：把 `libseekdb_cuvs_bridge.so` 换成基线版（原始 155 行 bridge），重复同一条命令。
**加速比 = 基线总耗时 / 优化版总耗时**。

---

## 5. 需要重新编译 seekdb 时（约 25–40 分钟，24 核）

```bash
nohup bash -c '
  HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm tools/hipvs_bridge/build.sh /workspace/bridge/libseekdb_cuvs_bridge.so &&
  cmake -S . -B build_release -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON \
    -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
    -DCUVS_BRIDGE_LIB=/workspace/bridge/libseekdb_cuvs_bridge.so &&
  cmake --build build_release --target seekdb -j24
' > /workspace/build.log 2>&1 &
```

`build_release/src/observer/seekdb` 对 bridge 是动态链接，**换 .so 不必重编 seekdb**（先用 `ldd` 确认）。

---

## 6. 决策表

| 组合 | 预期构建加速 | Recall 风险 | 备注 |
| --- | --- | --- | --- |
| fp32 默认（128/256/100） | 1.0x | 0 | 基线 |
| fp16 默认 | ~1.5–1.8x | 极低（实测过 ±0.001） | 最先验证 |
| fp16 + intermediate 128 | ~2.2–2.6x | 低 | 候选提交 |
| fp16 + degree 96/intermediate 128/niter 60 | ~2.8–3.4x | 中 | 必须实测 recall |
| fp16 + degree 64/intermediate 96/niter 40 | ~3.5–4.5x | 中高 | SIFT ≥0.995 才可用 |

最终把选定参数写成 **bridge 里的默认值**（评测不设 env），env 只保留手动覆盖。

---

## 7. 收尾（务必）

```bash
cp /workspace/*.log /workspace/bridge/*.so /workspace/summary.json /mnt/ 2>/dev/null
# 容器非持久化：结果当天必须同步出来，scp/下载都行
```

产出用于报告：CPU/GPU 构建对比、各参数组合的构建耗时与 Recall、端到端 DDL 计时、显存峰值。
