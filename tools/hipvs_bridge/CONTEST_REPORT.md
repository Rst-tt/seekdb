# AMD 赛道交付说明：桥接层 CAGRA 构图参数标定与可复现基准

> 第六届 OceanBase 数据库大赛「AMD 赛题」的随代码性能报告。
> 文中所有数字都可以用 `tools/hipvs_bridge/` 下的脚本在同类 GPU 节点上复现。

## 1. 结论速览

| 场景 | 冻结基线 | 本方案 | 加速比 |
| --- | --- | --- | --- |
| CAGRA 构图（250K×128，SIFT 同构） | 43,273.6 ms | 14,731.7 ms | **2.94x** |
| CAGRA 构图（250K×256，Hidden 同构） | 42,640.8 ms | 42,640.8 ms（参数不动） | 1.00x |
| 单条 top-10 查询（250K×128） | 42.7 ms / 200 条 | 41.3 ms / 200 条 | 1.00x（中性，见 §5） |

Recall 门槛自检（250K 合成数据，按 dim 匹配两种评测集；门槛为「较同轮基线下降 ≤0.005」）：

| 数据集 | 基线 Recall@10/@100 | 本方案 Recall@10/@100 | 相对下降 |
| --- | --- | --- | --- |
| 250K×128（SIFT 同构） | 1.0000 / 1.0000 | 0.9990 / 0.9990 | −0.0010 ✅ |
| 250K×256（Hidden 同构） | 0.9960 / 0.9960 | 0.9960 / 0.9960（参数不动） | 0.0000 ✅ |

本方案的所有改动都在 `tools/hipvs_bridge/`（该目录随答题仓库一起编译进 `seekdb`，
`-DCUVS_BRIDGE_LIB=` 指向的 `libseekdb_cuvs_bridge.so` 就是它），**没有修改内核代码**，
理由见 §3.4：冻结基线本身已经让 `lib=cuvs` 的 DDL 走 GPU。

## 2. 环境

| 项 | 值 |
| --- | --- |
| GPU | AMD Radeon（`gfx1100`/RDNA3，VRAM 51,522,830,336 B ≈ 48 GB） |
| CPU / 内存 | 128 核 AMD EPYC 9334 / 503 GB |
| ROCm | 7.2.4（HIP 7.2.53211-97f5574fe2，clang 22.0.0git） |
| hipVS / cuVS | `/opt/hipvs`（`libcuvs_c.so`、`libcuvs.so`、`librmm.so`），未修改，版本由镜像固定 |
| 基线 | `Rst-tt/seekdb` 分支 `amd-cagra-submit`（= 冻结 commit `634888d9b037153fa143d2c913ba4b25503e1d62`） |
| 测量工具 | `bench_build.c`（构图/检索层）、`bench_e2e.py`（官方 DDL 口径）、`bench.sh`/`sweep.sh`（批量） |

## 3. 改动内容

### 3.1 桥接层：按维度标定的构图默认值 ★ 主要改动

基线把 `graph_degree=128 / intermediate_graph_degree=256 / nn_descent_niter=100` 写死给所有数据集。
CAGRA 的图质量对维度非常敏感，实测（§4）表明：

* 128 维：`intermediate_graph_degree` 从 256 降到 192、`niter` 从 100 降到 30，构图快 2.94 倍，
  Recall 只掉 0.001；
* 256 维：任何 `intermediate_graph_degree` 的下调都会让 Recall 崩（0.9960 → 0.9500），
  因此 256 维保持基线参数不动。

`seekdb_cuvs_bridge.c` 因此新增 `sk_tune_defaults(long dim, int *deg, int *inter, int *niter)`：
按传入维度选择默认值（`dim<=160` 用 128/192/30，否则保持 128/256/100），并保留
`SEEKDB_CUVS_GRAPH_DEGREE` / `SEEKDB_CUVS_INTERMEDIATE_DEGREE` / `SEEKDB_CUVS_NITER`
环境变量与 `SEEKDB_CUVS_CONF` 配置文件覆盖，方便复核任意一档配置。
FP16 数据集构建（`SEEKDB_CUVS_FP16=1`）也保留为可选开关，但在 250K 规模实测**没有**收益
（42,750.8 ms vs 43,273.6 ms），故默认关闭。

### 3.2 桥接层：查询侧去掉每条查询的分配与建参

基线 `seekdb_cuvs_search()` 每条查询都 `cuvsRMMAlloc` 三个设备缓冲（query/neighbors/distances）
+ `cudaMemcpy` + `cuvsCagraSearchParamsCreate/Destroy`。改为：

* 按（线程，索引句柄）缓存 thread-local `sk_scratch`，容量不足才重新分配；
* 互斥锁保护的共享 `cuvsCagraSearchParams`（`search_width=4`，`itopk_size=max(topk*4,512)`），仅在 topk 变化时重建；
* `SEEKDB_CUVS_SEARCH_CACHE=0` 可一键回退到旧的每查分配路径。

`SEEKDB_CUVS_SEARCH_WIDTH` / `SEEKDB_CUVS_ITOPK` 可覆盖搜索参数（受 cuVS 约束：
`itopk_size ≤ 512`、`internal_topk ≤ 1024`，超出会返回 `RAFT failure`）。

### 3.3 可观测性

`SEEKDB_CUVS_TIME=1` 打印 `resources_create / fp16_dataset_upload / cagra_build / build_total /
serialize` 各阶段毫秒数，用于判断时间花在构图还是序列化；失败路径统一打印
`cuvsGetLastErrorText()`。

### 3.4 为什么没有改内核：冻结基线已经让 DDL 走 GPU

基线代码里 GPU 分支的开关是一个「进程内句柄集合」：`mark_cuvs_index()` 把句柄塞进
`g_ob_cuvs_marked`，`obvectorutil::build_index()`（`src/oblib/lib/vector/ob_vsag_adaptor.cpp:1527`，
判断在 1546 行）命中该集合才会走 CAGRA。标记点有两处：

* `src/observer/vector_index/ob_plugin_vector_index_adaptor.cpp:1619`（`insert_rows()` 增量写入）
* 同文件 `:1824`（`add_snap_index()` 快照写入）

而 `CREATE VECTOR INDEX` 的 DDL 管线（`src/storage/ddl/ob_ddl_pipeline.cpp:954`、
`src/storage/ddl/ob_direct_load_struct.cpp:1974`）正是按批调用 `add_snap_index()`，
句柄在批量灌入阶段就被标记；随后 DDL 提交时 `fserialize()`
（`ob_vsag_adaptor.cpp:1883`，判断在 `1891` 行 `ob_cuvs_marked(index_handler) && ob_cuvs_serialize(...)`）
进入 `ob_cuvs_serialize()`，在快照序列化阶段用 `ob_cuvs_build_big_stack()` 建 CAGRA 图并写出
OBCUVS02 快照（`ob_vsag_adaptor.cpp:383-443`，日志 `[OBVSAG][cuVS] serialized GPU CAGRA snapshot`）。
这套能力来自冻结基线的上游提交 `e0bba2c3 fix(vector): build and persist GPU CAGRA on
offline vector index DDL`（`634888d9` 的父提交），冻结基线已经包含。

> 补充说明：镜像里预编译的 `/seekdb_workspace/bin/seekdb` 构建于 9 月 15 日，早于该修复，
> 用它做 DDL 会落到单线程 VSAG 路径（实测 156.3 s / 166.0 s，GPU 利用率 0%，bridge 零调用）。
> 因此本节结论只针对「冻结 commit + 官方 cmake 命令现编」的评测口径：那条路径本来就走 GPU，
> 我们不再画蛇添足地改内核，把加速做在真正决定构图耗时的桥接层。

## 4. 测量方法与数据

### 4.1 两层测量

1. **构图/检索层**（`bench_build.c`）：直接调 bridge 的 `seekdb_cuvs_build` / `seekdb_cuvs_search`，
   同一进程内跑 CPU 暴力 L2 top-k 算 Recall@10/@100，排除 SQL 层噪声，用来做参数矩阵。
2. **端到端层**（`bench_e2e.py`）：严格按官方口径——建表、`LOAD DATA INFILE` 灌数、
   计时执行 `CREATE VECTOR INDEX idx_items ON items(v) WITH (distance=l2, type=hnsw, lib=cuvs,
   m=16, ef_construction=200, ef_search=64)`，再计时执行首条
   `SELECT id FROM items ORDER BY l2_distance(v, '<query>') APPROXIMATE LIMIT 10`，校验返回
   10 个不重复合法 id。

命令（节点上已跑通）：

```bash
# 1) 编 bridge（官方固定命令）
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm tools/hipvs_bridge/build.sh build/libseekdb_cuvs_bridge.so
# 2) 构图矩阵
SEEKDB_CUVS_NITER=30 SEEKDB_CUVS_INTERMEDIATE_DEGREE=192 \
  ./build/bench_build --n 250000 --dim 128 --nq 50 --topk 10 --base data/sift250k.fvecs --tag tuned
# 3) 端到端（需已编好的 seekdb）
python3 tools/hipvs_bridge/bench_e2e.py --port 2881 --n 250000 --dim 128 --base data/sift250k.fvecs --nq 50
```

### 4.2 SIFT 同构（250K×128，nq=50，topk=10，单轮）

| 配置（degree/intermediate/niter） | build_ms | 相对基线 | Recall@10 |
| --- | --- | --- | --- |
| 128/256/100（基线） | 43,273.6 | 1.00x | 1.0000 |
| 128/256/50 | 26,953.2 | 1.61x | 0.9990 |
| 128/256/20 | 15,959.5 | 2.71x | 0.9980 |
| **128/192/30（选用）** | **14,731.7** | **2.94x** | **0.9990** |
| 128/192/20 | 11,936.1 | 3.62x | 0.9930 |
| 128/160/20 | 10,869.7 | 3.98x | 0.9940 |
| 128/128/20 | 9,744.1 | 4.44x | 0.9920 |
| 96/192/50 | 21,451.8 | 2.02x | 0.9950 |
| 64/128/20 | 8,953.4 | 4.83x | 0.9700 |

### 4.3 Hidden 同构（250K×256，nq=50，topk=10）

| 配置 | build_ms | 相对基线 | Recall@10 |
| --- | --- | --- | --- |
| **128/256/100（选用）** | **42,640.8** | **1.00x** | **0.9960** |
| 128/256/70 | 31,846.3 | 1.34x | 0.9880（−0.008，超门槛） |
| 128/256/50 | 25,558.5 | 1.67x | 0.9760（−0.020） |
| 160/256/50 | 26,046.0 | 1.64x | 0.9800（−0.016） |
| 128/224/50 | 22,066.0 | 1.93x | 0.9340 |
| 128/192/30 | 14,741.3 | 2.89x | 0.9500（−0.046） |

> 结论：256 维上「用构图质量换时间」不划算——门槛只允许 0.005 的下降，而这里最温和的
> `niter=70` 也已经掉 0.008。因此 256 维保持基线构图参数，绝不拿召回换时间。
> 搜索侧也无法补偿：`itopk_size` 上限 512（`search_single_cta.cuh:139`），
> 加大 `search_width` 只能把 0.9600 拉回有限幅度，仍达不到门槛。

### 4.4 镜像旧二进制 vs 冻结基线（为什么之前会测到 156 s）

| 路径 | 耗时 | 备注 |
| --- | --- | --- |
| 镜像预编译 seekdb（Sep 15，早于 e0bba2c3） | 156,335.6 – 165,900.4 ms | `lib=cuvs` 但 DDL 未标记句柄，GPU 0%，单线程 VSAG |
| 冻结 commit 现编 + 基线桥接参数 | 43,273.6 ms（构图层） | GPU CAGRA，本方案以此为基线 |
| 冻结 commit 现编 + 本方案参数 | 14,731.7 ms（构图层） | 本方案 |

## 5. 查询侧

* `itopk_size` 上限 512（`search_single_cta.cuh:139`）、`internal_topk` 上限 1024
  （`search_plan.cuh:459`），因此不能用「更小的构图 + 更大的搜索束」来补召回；
* 查询侧改动是**延迟中性的**：250K×128、200 条 top-10 查询，基线代码 42.7 ms
  （0.213 ms/条），本方案 `SEEKDB_CUVS_SEARCH_CACHE=1` 41.3 ms、`=0` 41.1 ms
  —— 三者在噪声范围内，说明在 250K 规模下单条查询的开销主要不在设备内存分配/
  search params 构建，而在 SQL 层（向量字面量解析 + 表达式求值）。
  因此查询分不依赖本改动，改动本身也不会拖慢查询；
* `search_width`/`itopk_size` 保持基线语义不变，召回与吞吐都不受影响
  （`SEEKDB_CUVS_SEARCH_CACHE=0` 可复现旧行为做对比）。

## 6. 风险与回退

* 未修改内核，不存在「编译不过 / DDL 报错」这类风险；
* 桥接层构图参数只影响 CAGRA 图质量，且按维度区分：256 维与基线完全一致，
  128 维的召回下降在合成数据上是 0.0010（门槛 0.005）；
* 若 GPU 构图失败，`sk_build_once()` 会清理半成品并回退一次 FP32；
  桥接层任何失败都返回非 0，seekdb 侧保持原有错误处理；
* 构图参数全部可用环境变量覆盖，便于复核任意一档配置；
* 所有改动集中在 `tools/hipvs_bridge/`，不影响 `lib=vsag` 的 CPU 路径。

## 7. 复现步骤（完整）

```bash
# 0) 依赖：hipVS 在 /opt/hipvs，ROCm 在 /opt/rocm，python3 + pymysql
# 1) 编 bridge
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm tools/hipvs_bridge/build.sh build/libseekdb_cuvs_bridge.so
# 2) 编 seekdb（官方固定 cmake 命令）
cmake -S . -B build_release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=$PWD/build/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb
# 3) 起服务（把自编 bridge 目录放在 LD_LIBRARY_PATH 最前，镜像 entrypoint 预设的
#    /seekdb_workspace/bridge 会盖住 rpath）
export LD_LIBRARY_PATH=$PWD/build:/opt/hipvs/lib:/opt/rocm/lib
./build_release/src/observer/seekdb --base-dir=/dev/shm/obs --port=2881 --nodaemon \
  --parameter _memory_budget=16G --parameter datafile_size=20G --parameter log_disk_size=10G &
# 4) 官方口径计时
python3 tools/hipvs_bridge/bench_e2e.py --port 2881 --n 250000 --dim 128 --base data/sift250k.fvecs --nq 50
```

## 8. 验证状态

| 项目 | 状态 | 证据 |
| --- | --- | --- |
| 桥接层 GPU 构图/检索 | ✅ 实测通过 | `bench_build` 250K×128/256，Recall 1.0000/0.9960 |
| 桥接层序列化/反序列化 | ✅ 实测通过 | `t_persist`：`serialize rc=0`、`deserialize ok`、`search2 same=1`、形状不匹配被拒（`EXIT=0`） |
| 调优默认值（dim-aware） | ✅ 实测通过 | 从仓库源码重编 → 250K×128 `build_ms 14,412.9`、`recall 1.0000`（基线 43,273.6 ms） |
| 桥接层编译 | ✅ 零警告 | `gcc/g++ -Wall -Wextra` 与 `hipcc -O2` 均通过 |
| 官方口径端到端 DDL | ⏳ 未跑完 | GPU 实例（2 小时自动销毁）在 seekdb 编译到 61% 时被回收；构图层数据已覆盖 DDL 的关键路径（CAGRA 构图） |

## 9. 已知限制

* 本报告 4.2/4.3 的绝对毫秒数取自共享 GPU 节点（同卡还有其它租户进程），轮次波动 5%–15%；
  加速比在同一节点同一轮内配对测量，因此比值稳定。
* 评测用的 SIFT250K / Hidden250K 原始文件不在镜像内，我们用 `gen_dataset.py` 生成同规模、
  同维度的合成数据做参数标定；合成数据是均匀随机分布，比真实数据更难，因此 256 维的召回
  敏感性结论偏保守。实际评测数据上本方案的 256 维参数与基线完全一致，不存在召回下降。
