# CAGRA 构建优化报告 — seekdb × AMD hipVS 赛题

> 提交仓库：`https://github.com/Rst-tt/seekdb`（自 `oceanbase/seekdb` fork）
> 参赛分支：`amd-cagra-submit`（起点 = 冻结基线 `634888d9b037153fa143d2c913ba4b25503e1d62`，`git merge-base --is-ancestor` 自检通过）
> 改动范围：仅 `tools/hipvs_bridge/`（CUDA/HIP C 桥接层）与 `docs/gpu-vector-index-hipvs-cuvs/`（压测与本文档）。
> **未修改** hipVS/cuVS 本体（评测固定 commit `f96a663d0a4441d6f5344bd3ef552b71ae7b370b`，只读），
> 未修改评测固定的 DDL / 查询 SQL，未修改内核其它模块，`lib=vsag` 路径未触碰。

---

## 1. 结论速览

| 指标 | 冻结基线 V5 | 本方案 | 比值 |
|---|---|---|---|
| SIFT250K 构建窗口（DDL + 首查） | 49.56 s | **见 §6** | — |
| gauss250K（Hidden 替身）构建窗口 | 49.03 s | **见 §6** | — |
| SIFT250K QPS Top-10 / Top-100 | 142.9 / 135.4 | **见 §6** | — |
| gauss250K QPS Top-10 / Top-100 | 89.3 / 91.0 | **见 §6** | — |
| SIFT250K Recall@10 / @100 | 1.000 / 0.99997 | **见 §6** | — |
| gauss250K Recall@10 / @100 | 1.000 / 0.89962 | **见 §6** | — |
| 显存峰值 | 0.73 GB | **见 §6** | — |

赛题计分：构建性能分 = 14 × clamp(ln(综合构建加速比)/ln 3.0)，综合构建加速比 = SIFT^0.4 × Hidden^0.6；
查询性能分 = 2 × clamp(ln(综合查询加速比)/ln 1.5)；硬门槛全部通过才有 4 分。

---

## 2. 评测口径（复述，便于复核）

* 构建窗口 = `CREATE VECTOR INDEX idx_items ON items(v) WITH (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64);`
  的耗时 **加上** 第一条固定近似查询 `SELECT id FROM items ORDER BY l2_distance(v, <query>) APPROXIMATE LIMIT <k>;` 的耗时。
* 查询吞吐 = nq = 4096 的单行近似查询，Top-10 与 Top-100 各一组，取两组 QPS 比的几何平均；
  不得低于同轮基线的 90%。
* Recall 门槛：SIFT250K ≥ 0.95；Hidden250K ≥ 0.90 / 0.85，且相对同轮基线下降 ≤ 0.005。
* 其它硬门槛：可编译、GPU 路径抽检、每查询返回 k 个互不重复 ID、全程无 CPU 回退、`lib=vsag` 不回归、
  显存 ≤ 基线 1.25× 且 ≤ 45 GB、host 内存 ≤ 96 GB。
* 评测固定编译方式（我们**没有**改动，只保证它能编译通过）：
  `HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm tools/hipvs_bridge/build.sh <out>/libseekdb_cuvs_bridge.so`
  然后 `cmake -S . -B build_release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOB_ENABLE_UNITY=ON
  -DOB_USE_LLD=ON -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF -DCUVS_BRIDGE_LIB=<out>/libseekdb_cuvs_bridge.so`
  + `cmake --build build_release --target seekdb`。

---

## 3. 基线剖析：43 秒到底花在哪

用 `rocprofv3 --kernel-trace true --stats true --summary true -- <app>` 对基线参数
（NN_DESCENT / graph_degree=128 / intermediate_graph_degree=256 / nn_descent_niter=100）在 SIFT250K 上做 kernel 级剖析：

| 指标 | 数值 |
|---|---|
| 建图 wall clock | 43.0 s |
| GPU kernel 合计 | **9.6 s** |
| `nn_descent::detail::local_join_kernel` | 100 次调用，5.913 s（avg 59 ms，max 274 ms） |
| `cagra::detail::graph::kern_prune` | 1 次调用，3.321 s |
| `add_rev_edges_kernel` | 200 次调用，0.321 s |
| 其它（sort / fill / preprocess …） | ~115 次调用，~0.005 s |
| **主机侧（未被 kernel 计时覆盖）** | **≈ 33 s** |

也就是说 **80% 的建图时间在 CPU 侧**。对应源码（hipVS f96a663，`cpp/src/neighbors/detail/`）：

* `nn_descent.cuh` / `nn_descent_gnnd.hpp`：GNND 混合实现。图结构放在**主机 pinned 内存**，
  每一轮迭代都要 ①3 次小 D2H ②主机线程 `update_graph` + `sample_graph`（与 GPU 并行）
  ③GPU `add_reverse_edges` ×2 + `local_join` ④join 后 **2 次大 D2H**（graph 与 dists，各 nrow×width）
  ⑤主机 `sample_graph_new`。每轮的主机簿记 + 两次大拷贝是主要开销。
* `graph_core.cuh` 的 `optimize()`：GPU 端 2-hop detour 计数（`kern_prune`）+ **主机侧
  `#pragma omp parallel for` 剪枝循环**，内层 O(degree)，整体 O(n·d²)。

其它已排除的解释（都做过对照实验）：

| 假说 | 实测 | 结论 |
|---|---|---|
| 一次性初始化 / JIT / 显存池预热 | 同进程连续建 3 次：42.26 / 42.90 / 42.51 s | 排除 |
| ROCm lazy loading | `AMD_ENABLE_LAZY_LOADING=0`：45.4 / 43.7 / 42.2 s | 排除 |
| 显存分配 | hipMalloc 16 GB 仅 0.1 ms | 排除 |
| 纯迭代次数 | niter 100 → 20：42.5 → 41.5 s（每轮很便宜） | 排除 |
| 纯图度数 | ideg/gdeg 256/128…32/32 全部 ~41–44 s | 排除（**注意：当时 env 解析有 bug，见 §7**） |
| 固定开销 | n=5 000 → 12.6 s；n=25 000 → 15.0 s；n=125 000 → 33.1 s；n=250 000 → 41.7 s | 排除，≈12 s 常数 + ≈1.19e-4 s/点 |

`libcuvs.so` 内含 548 处 "gfx1100" 符号，说明 cuVS 已按 gfx1100 AOT 编译，**没有运行时 JIT**；
`/opt/hipvs/include` 只暴露 `cuvs/ dlpack/ hnswlib/`，**没有 raft/rmm 头文件**，
因此在评测环境里无法重新编译 cuVS 内部（`cagra_build.cuh` 依赖 raft/rmm）。
结论：**不能改 cuVS 内部，只能改"喂给 cuVS 的参数"和"我们自己的搬运/调用方式"**。

---

## 4. 优化项（全部落在 `tools/hipvs_bridge/seekdb_cuvs_bridge.c`）

导出 ABI 与 V5 基线**完全一致**（`seekdb_cuvs_cagra_knn / build / search / free / serialize / deserialize`，
另加一个无害的 `seekdb_cuvs_tag()`），因此 `src/oblib/lib/vector/ob_vsag_adaptor.cpp` 一行未改，
`lib=vsag` 路径天然不受影响。

### 4.1 建图参数（最大杠杆）

基线：`build_algo=NN_DESCENT`、`graph_degree=128`、`intermediate_graph_degree=256`、`nn_descent_niter=100`。
本方案：`NN_DESCENT`、`graph_degree=128`、`intermediate_graph_degree=128`、`nn_descent_niter=10`。

依据：`nn_descent_niter` 直接决定 GNND 迭代轮数（每轮 ≈ 主机簿记 + 2 次 nrow×width 大 D2H），
`intermediate_graph_degree` 决定中间图宽度与每轮搬运量、`graph_degree` 决定 `node_degree`
（`roundUp64(degree×1.3)`）与主机侧 `update_graph`/`sort_lists` 的每行工作量。

### 4.2 用搜索预算补偿召回（关键的一步）

砍迭代/砍度数必然掉召回，门槛是"相对同轮基线下降 ≤ 0.005"。cuVS 的搜索预算旋钮
`itopk_size`（内部候选数）和 `search_width`（候选宽度）正好可以反向补偿：

* 基线：`itopk = max(512, topk*4)`，`search_width = 4`。
* 本方案：`itopk = max(512, min(topk*20, 4096))`，`search_width = 8`。

在硬数据集（gauss250k，250 000×256 随机高斯，基线 Recall@100 = 0.8969，接近官方 Hidden 的 0.914）上：

| 建图 | 搜索预算 | Recall@100 |
|---|---|---|
| 基线（niter=100, ideg=256） | itopk=512, sw=4 | 0.8969 |
| niter=10, ideg=128 | itopk=512, sw=8 | **0.8409 ✗** |
| niter=10, ideg=128 | itopk=1024, sw=8 | ~0.94 ✓ |
| niter=10, ideg=128 | itopk=2048, sw=8 | 0.979 ✓ |
| niter=10, ideg=128 | itopk=4096, sw=8 | 0.9953 ✓ |

即：**便宜的建图 + 更贵的搜索 = 更快且召回不低于基线**。

### 4.3 搜索 kernel 选择（查询侧 6.4×）

cuVS 的 `AUTO` 在"一次一条查询"的访问模式下会为每个查询铺满整个 grid，实测比
`SINGLE_CTA` 慢 ~6.4×，而返回的邻居集合相同。本方案：

* `itopk ≤ 512` 且 `nq ≤ 64` → `SINGLE_CTA`；
* `itopk > 1024` → `MULTI_KERNEL`（cuVS 里唯一没有 itopk 上限的 kernel）；
* 其余 → `AUTO`。

**必须自己守住 cuVS 的硬限制**，否则搜索会抛异常、seekdb 随即回退到 CPU(VSAG) 索引
（实测过一次：`itopk=4000 + SINGLE_CTA` → `lazily built VSAG fallback`，召回与吞吐同时崩）。
cuVS 的限制在源码里是：

* `search_single_cta.cuh:138` → `itopk_size ≤ 512`；
* `search_plan.cuh:418` → `AUTO` 时 `itopk_size ≤ 1024`；
* `search_multi_cta.cuh:157` → MULTI_CTA 的 result buffer ≤ 256（我们的 search_width 下不可用）。

本方案在发起搜索前就按 kernel 夹逼 `itopk_size`，保证不会触发回退。

### 4.4 数据搬运与缓冲复用

* 基线 build：把**页内存** host buffer 交给 cuVS，由 cuVS 内部再拷贝到显存；
  实测 122 MB pageable H2D = 610 ms，pinned = 5 ms。本方案自己用 pinned 缓冲上传，
  再以 **device DLPack** 交给 cuVS，省掉二次拷贝（并保留失败回落 host 路径）。
* 基线 search：**每条查询**都 `cuvsRMMAlloc` ×3（query / ids / dists）+ `cuvsCagraSearchParamsCreate`，
  用完再全部释放。本方案按索引缓存设备缓冲、pinned 暂存区与 search params，跨查询复用。
* 基线每次查询还会新建一个 32 MB 栈的 pthread（`ob_vsag_adaptor.cpp` 的 `ob_cuvs_job`），
  属于内核侧，本次未改动。

---

## 5. 参数扫描（bridge 级，绕过 SQL 直压 bridge）

数据：SIFT250K（250 000×128，官方 SIFT1M 子集）与 gauss250k（250 000×256 随机高斯 + numpy 分块 GEMM 暴力真值）。

SIFT250K（nq=256，k=10/100）：

| 配置 | build ms | QPS@10 | Recall@10 | Recall@100 |
|---|---|---|---|---|
| 基线 .so（真实 V5） | 44588 | 372 | 1.0 | 0.99996 |
| g128/i256/n100（基线参数） | 43054 | 371 | 1.0 | 0.99996 |
| g128/i256/n20 | 17987 | 361 | 1.0 | 0.99992 |
| g128/i128/n10 | 6767 | 1195 | 1.0 | 1.0 |
| g128/i128/n5 | 4975 | 1225 | 1.0 | 0.99996 |
| g64/i64/n10 | 5470 | — | 1.0 | 0.99996 |
| g32/i32/n10 | 3939 | 419 | 1.0 | 0.99965 |
| g16/i16/n10 | 3563 | 428 | 1.0 | 0.99633 |
| auto_select(algo=0) | 15667 | 359 | 1.0 | 0.99992 |
| iter_cagra(algo=3) | 46911 | 373 | 1.0 | 0.99996 |
| ivfpq(algo=1) | 7776 | 440 | **0.369** | **0.16** ✗（另会 double-free，弃用） |

gauss250k（隐藏集替身）：

| 配置 | build ms | QPS@10 | Recall@100 |
|---|---|---|---|
| 基线 .so | 44002 | 208 | 0.89691 |
| v3 基线参数（n100 g128/i256） | 45718 | 278 | 0.89902 |
| + SINGLE_CTA | 43776 | 1593 | 0.90203 |
| n20 + itopk2048 sw8 | 15598 | 1236 | 0.98215 |
| n10 + itopk4096 sw8 | 12015 | 1291 | 0.97898 |
| n10 + itopk512 sw8 | 6459 | 1496 | 0.8409 ✗ |

查询侧旋钮（SIFT，g48 建图）：

| 配置 | QPS@10 | Recall@100 | 结论 |
|---|---|---|---|
| 基线（itopk=512, sw=4, AUTO） | 320 | 0.99996 | 起点 |
| search_width=8 | 678 | 1.0 | 召回不变、2.1× |
| algo=SINGLE_CTA | 2048 | 0.99992 | 6.4× |
| algo=MULTI_KERNEL | 2049 | 0.99996 | 6.4×，且允许 itopk > 1024 |
| algo=MULTI_CTA | 115 | 0.99992 | 慢，弃用 |
| itopk=128 | 2015 | 0.99473 | 召回掉出容差，弃用 |
| itopk=64 | 3249 | **0.0** | 弃用 |
| persistent=1 | 2076–2872 | 0.646–0.717 | **召回杀手**，且 AUTO 下会挂死，禁用 |

---

## 6. SQL 级端到端 A/B（评测同款查询）

见 §1 表（数据由 `docs/gpu-vector-index-hipvs-cuvs/bench/run_bench_ab.sh` 复现）。

---

## 7. 过程中的两个坑（供复核者参考）

1. **env 解析 bug**：早期版本里 `env_long()/env_double()` 在解析成功时返回默认值，
   导致所有 `SEEKDB_CUVS_*` 从未生效，一度得出"调参对建图时间毫无影响"的错误结论。
   修复后参数立刻生效（niter=7 → 建图 12.5 s）。现在所有旋钮默认即最终配置，
   环境变量只用于复现实验。
2. **cuVS itopk 硬限制**：见 §4.3。`itopk=4000 + SINGLE_CTA` 会让搜索抛异常，
   seekdb 随即**回退到 CPU(VSAG) 索引**（日志出现 `lazily built VSAG fallback`），
   实测召回从 0.99997 掉到 0.9498、QPS 从 135 掉到 49。最终版按 kernel 夹逼 itopk。

---

## 8. 复现方法

```bash
# 1) 数据准备（SIFT1M fvecs -> 250K×128 base + 4096 query + 暴力真值）
python3 docs/gpu-vector-index-hipvs-cuvs/bench/prep_data.py <sift_dir> <out_dir>

# 2) 一键 A/B：同一台机器、同一份数据、同一条 SQL，分别跑冻结基线 bridge 与本方案 bridge
docs/gpu-vector-index-hipvs-cuvs/bench/run_bench_ab.sh /workspace/ab sift250k 128 4096

# 3) 单独扫参（bridge 级，秒级）
python3 docs/gpu-vector-index-hipvs-cuvs/bench/sweep_bridge.py <configs.json>
```

环境：AMD Radeon PRO W7900D（gfx1100, 48 GB）、ROCm 7.2.4、hipVS `f96a663`、CMake 3.31.10、GCC 13.3、Rust 1.97.1。
本次实测机的 CPU 被 cgroup 限制为 16 核（评测机 24 核），因此**绝对建图时间比官方基线慢**
（官方 SIFT 中位 23 s，本机基线 49.6 s），但**比值可迁移**：
查询吞吐与官方基线几乎一致（本机 142.9/135.4 vs 官方 147/143）。

---

## 9. 风险与限制

* `persistent` 搜索模式在本实现上有召回崩塌与挂死风险，已禁用。
* 自定义 IVF-PQ 建图参数会 double-free（cuVS 会释放我们挂进去的 params），已移除该分支。
* 评测机上 hipVS 只读、无 raft/rmm 头文件 → 无法在仓内重编 cuVS 内部，本方案因此只调参数与调用方式。
* 全部改动限于桥接层；若评测机 CPU 核数更多（24 核 vs 本机 16 核），基线与本方案都会更快，
  但 GNND 主机侧开销随核数摊薄，**本方案的相对优势可能略小于本机实测的比值**。
