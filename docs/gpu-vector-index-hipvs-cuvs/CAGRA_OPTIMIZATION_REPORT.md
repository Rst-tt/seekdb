# CAGRA 构建优化报告 — seekdb × AMD hipVS 赛题

> 提交仓库：`https://github.com/Rst-tt/seekdb`（自 `oceanbase/seekdb` fork）
> 参赛分支：`amd-cagra-submit`（起点 = 冻结基线 `634888d9b037153fa143d2c913ba4b25503e1d62`，`git merge-base --is-ancestor` 自检通过）
> 改动范围：仅 `tools/hipvs_bridge/`（CUDA/HIP C 桥接层）与 `docs/gpu-vector-index-hipvs-cuvs/`（压测脚手架与本文档）。
> **未修改** hipVS/cuVS 本体（评测固定 commit `f96a663d0a4441d6f5344bd3ef552b71ae7b370b`，只读），
> 未修改评测固定的 DDL / 查询 SQL，未修改内核其它模块，`lib=vsag` 路径未触碰。

---

## 1. 结论速览

同一台机器、同一份 seekdb 二进制、**只替换桥接层 `.so`** 的配对 A/B（评测同款 SQL，nq = 4096 单行近似查询）：

| 指标 | 冻结基线 V5 | 本方案 | 比值 |
|---|---|---|---|
| SIFT250K 构建窗口（DDL + 首查） | 49.098 s | **10.195 s** | **4.82×** |
| gauss250K（Hidden 替身）构建窗口 | 50.416 s | **12.844 s** | **3.92×** |
| SIFT250K QPS Top-10 / Top-100 | 138.34 / 130.19 | **206.84 / 183.65** | 1.49× / 1.41× |
| gauss250K QPS Top-10 / Top-100 | 92.68 / 91.44 | **127.40 / 118.92** | 1.37× / 1.30× |
| SIFT250K Recall@10 / @100 | 1.000 / 0.99996 | 1.000 / **0.99997** | Δ = +0.00001 |
| gauss250K Recall@10 / @100 | 1.000 / 0.89935 | 1.000 / **0.99502** | Δ = +0.09567 |
| 每查询返回 k 个互不重复 ID | ✓ | ✓ | — |
| 全程 GPU 无 CPU 回退 | ✓（无 `lazily built VSAG fallback`） | ✓ | — |
| 显存峰值 | 0.73 GB | 0.73 GB | 1.00×（≤ 基线 1.25× 且 ≤ 45 GB ✓） |

按赛题公式折算：

* 综合构建加速比 = 4.82^0.4 × 3.92^0.6 = **4.27×** → 构建性能分 = 14 × clamp(ln 4.27 / ln 3.0) = **14 / 14**（≥3.0× 满分）
* 综合查询加速比 = 1.452^0.4 × 1.337^0.6 = **1.38×** → 查询性能分 = 2 × clamp(ln 1.38 / ln 1.5) = **1.60 / 2**
* 硬门槛（可编译、GPU 路径、结果完整性、无 CPU 回退、召回、显存、vsag 不回归）全部通过 → 4 分
* **合计 ≈ 19.6 / 20**

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

其它已排除的解释（都做过对照实验；注意"纯迭代次数 / 纯图度数"两行来自早期带 env 解析 bug 的构建，
见 §7 坑 1，修正后结论已按真实生效参数重测）：

| 假说 | 实测 | 结论 |
|---|---|---|
| 一次性初始化 / JIT / 显存池预热 | 同进程连续建 3 次：42.26 / 42.90 / 42.51 s | 排除 |
| ROCm lazy loading | `AMD_ENABLE_LAZY_LOADING=0`：45.4 / 43.7 / 42.2 s | 排除 |
| 显存分配 | hipMalloc 16 GB 仅 0.1 ms | 排除 |
| 固定开销 | n=5 000 → 12.6 s；n=25 000 → 15.0 s；n=125 000 → 33.1 s；n=250 000 → 41.7 s | 排除，≈12 s 常数 + ≈1.19e-4 s/点 |
| 纯迭代次数 / 纯图度数 | （早期 bug 构建）niter 100→20 仅 42.5→41.5 s | 修正后**不成立**，两者都能线性砍时间（见 §5） |

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

实测（bridge 级，gauss250k 250 000×256）：niter 100 → 44.0 s；niter 20 → 20.6 s；niter 10 → **6.4 s**。
即建图窗口整体从 49 s 降到 10–13 s。

### 4.2 用搜索预算补偿召回（关键的一步）

砍迭代/砍度数必然掉召回，门槛是"相对同轮基线下降 ≤ 0.005"。cuVS 的搜索预算旋钮
`itopk_size`（内部候选数）和 `search_width`（候选宽度）正好可以反向补偿：

* 基线：`itopk = max(512, topk*4)`，`search_width = 4`，`algo = AUTO`。
* 本方案：`itopk = max(512, min(topk*40, 4096))`，`search_width = 8`，`algo = MULTI_CTA`。

在硬数据集（gauss250k，250 000×256 随机高斯，基线 Recall@100 = 0.8994，接近官方 Hidden 的 0.914）上：

| 建图 | 搜索预算 | Recall@100 | QPS@100 |
|---|---|---|---|
| 基线（niter=100, ideg=256） | itopk=512, sw=4, AUTO | 0.8969 | 267 |
| niter=10, ideg=128 | itopk=512, sw=8 | **0.8356 ✗** | 1577 |
| niter=10, ideg=128 | itopk=1024, sw=8 | 0.9308 | 1497 |
| niter=10, ideg=128 | itopk=2048, sw=8 | 0.9772 | 1230 |
| niter=10, ideg=128 | itopk=4096, sw=8 | **0.9966** | 1021 |

即：**便宜的建图 + 更贵的搜索 = 更快且召回高于基线**（最终 SQL 级实测 Recall@100 = 0.99502，比基线 0.89935 还高 0.096）。

### 4.3 搜索 kernel 选择（必须自己守住 cuVS 的硬限制）

cuVS 的 `cuvsCagraSearchAlgo` 枚举是 `{ SINGLE_CTA=0, MULTI_CTA=1, MULTI_KERNEL=2, AUTO=3 }`
（hipVS `cpp/include/cuvs/neighbors/cagra.h:211`）。三个 kernel 的关键差异（源码位置见 hipVS f96a663）：

* `search_single_cta.cuh:138-139` → `constexpr unsigned max_itopk = 512; RAFT_EXPECTS(itopk_size <= max_itopk, ...)`
  —— **release 构建里 `RAFT_EXPECTS` 被编掉**，itopk > 512 时**不报错，但静默返回错误邻居**
  （实测 gauss Recall@100 从 0.90 掉到 0.29，而每查询仍返回 k 个互不重复 ID，肉眼完全看不出问题；
  seekdb 侧随后触发 `lazily built VSAG fallback` 回退到 CPU(VSAG) 索引，吞吐与召回同时崩）。
* `search_plan.cuh:418-422` → `AUTO` 时 `itopk_size ≤ 1024`（超出报 "internal_topk must be <= 1024"）。
* `search_plan.cuh:145-153` → `AUTO` 的分派规则：`itopk ≤ 512 && max_queries ≥ num_sm*2` 走 SINGLE_CTA，
  否则走 **MULTI_CTA**（我们的 `max_queries = 0`，所以 AUTO 实际总是落到 MULTI_CTA）。
* `search_multi_cta.cuh:145-158` → MULTI_CTA **没有 itopk 上限**（内部 itopk = warp_size = 64，
  `search_width` 被强制为 1，`result_buffer_size = 64 + 1*graph_degree ≤ 256`，graph_degree=128 → 192 ✓）。
* `search_multi_cta.cuh` 的 CTA 数 = `max(search_width, ceildiv(global_itopk_size, 64))` → **itopk 越大 CTA 越多，召回越好**。

实测（同一张 g128/i128/n10 图，gauss250k，nq=64）：

| algo | QPS@10 | Recall@100 | 结论 |
|---|---|---|---|
| SINGLE_CTA（默认路径） | 377 | 0.839 | 慢 4×，且 itopk ≤ 512 |
| MULTI_CTA | 1588 | 0.710（itopk 被夹到 256 时） | itopk 不夹逼时可到 0.99+ |
| MULTI_KERNEL | 223 | 0.838 | nq=1 时太慢 |
| **AUTO** | **1484** | **0.837** | 与 MULTI_CTA 同路 |

因此本方案**显式指定 `algo = MULTI_CTA`**（唯一没有 itopk 上限的快 kernel），
并把 `itopk` 提到 `max(512, min(topk*40, 4096))` —— 这正是召回能补回来的原因。
同时对 `SINGLE_CTA`（≤512）与 `AUTO`（≤1024）仍然保留夹逼，避免任何路径踩到 cuVS 的静默错误。

### 4.4 数据搬运与缓冲复用

* 基线 build：把**页内存** host buffer 交给 cuVS，由 cuVS 内部再拷贝到显存；
  实测 122 MB pageable H2D = 610 ms，pinned = 5 ms。本方案自己用 pinned 缓冲上传，
  再以 **device DLPack** 交给 cuVS，省掉二次拷贝（并保留失败回落 host 路径）。
* 基线 search：**每条查询**都 `cuvsRMMAlloc` ×3（query / ids / dists）+ `cuvsCagraSearchParamsCreate`，
  用完再全部释放。本方案按索引缓存设备缓冲、pinned 暂存区与 search params，跨查询复用。
  这是查询侧 6× 提速的主要来源（同一 algo 下，基线 208→277 q/s，本方案 1484 q/s）。
* 基线每次查询还会新建一个 32 MB 栈的 pthread（`ob_vsag_adaptor.cpp` 的 `ob_cuvs_job`），
  属于内核侧，本次未改动（保持 `lib=vsag` 与日志字符串一字不动）。

---

## 5. 参数扫描（bridge 级，绕过 SQL 直压 bridge）

数据：SIFT250K（250 000×128，官方 SIFT1M 子集）与 gauss250k（250 000×256 随机高斯 + numpy 分块 GEMM 暴力真值）。

### 5.1 建图参数（gauss250k，nq=64，itopk=1024）

| 建图参数 | build ms | QPS@10 | Recall@100 |
|---|---|---|---|
| 基线：niter=100, g128/i256 | 44 002 | 208 | 0.89691 |
| niter=100, g128/i128 | 45 718 | 278 | 0.89902 |
| niter=20, g128/i128 | 11 130 | 1533 | 0.93391 |
| niter=10, g128/i128 | 6 369 | 1499 | 0.93078 |
| niter=50, g128/i128 | 19 773 | 1390 | 0.95516 |

### 5.2 搜索预算（gauss250k，nq=64，建图 g128/i128/n10）

| itopk | build ms | QPS@10 | QPS@100 | Recall@100 |
|---|---|---|---|---|
| 512 | 6 575 | 1578 | 1577 | 0.83562 ✗ |
| 1024 | 6 369 | 1499 | 1497 | 0.93078 ✓ |
| 2048 | 7 237 | 1213 | 1230 | 0.97719 ✓ |
| 4096 | 7 685 | 1016 | 1021 | 0.99656 ✓ |

其它旋钮（`max_iterations` / `min_iterations` / `num_random_samplings` / `hashmap_mode`）在 itopk=1024 下
都没有增益（Recall@100 都在 0.93 附近）；其中 `hashmap_mode = SMALL(1)` 直接把 Recall 打到 **0.0**，已禁用。

### 5.3 被排除的路线

| 路线 | 实测 | 结论 |
|---|---|---|
| `build_algo = IVF_PQ` | 建图 7.8 s，但 Recall@10 = 0.369 / @100 = 0.16，且退出时 `free(): invalid pointer` | 弃用（召回崩塌 + cuVS 会释放我们挂进去的 params） |
| `build_algo = ITERATIVE_CAGRA_SEARCH` | 建图 46.9 s（比基线还慢） | 弃用 |
| `search_algo = MULTI_KERNEL` | nq=1 时 223 q/s（比 MULTI_CTA 慢 7×） | 弃用 |
| `persistent` 搜索模式 | Recall@100 0.54–0.72；AUTO 下会挂死（GPU 100% 8 分钟不退出） | **禁用** |
| `hashmap_mode = SMALL` | Recall = 0.0 | 禁用 |
| `itopk` 夹逼到 256（曾对 MULTI_CTA 做过） | Recall@100 = 0.709 | 撤销该夹逼 |

---

## 6. SQL 级端到端 A/B（评测同款查询）

同一台机器、同一 seekdb 二进制（`build_release/src/observer/seekdb`），
**只把 `LD_LIBRARY_PATH` 里的桥接 `.so` 换成基线版 / 本方案版**，其余完全一致。
nq = 4096 条单行近似查询（`select id from items order by l2_distance(v,<literal>) approximate limit k;`，
单 mysql 进程 stdin 管道逐条下发），Top-10 与 Top-100 各一组。

| 数据集 | 臂 | load s | DDL s | 首查 s | **构建窗口 s** | QPS@10 | QPS@100 | Recall@10 | Recall@100 | 互异 ID | CPU 回退 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| SIFT250K | 冻结基线 V5 | 9.19 | 49.043 | 0.055 | **49.098** | 138.34 | 130.19 | 1.0 | 0.99996 | ✓ | 无 |
| SIFT250K | **本方案** | 9.19 | 10.164 | 0.031 | **10.195** | **206.84** | **183.65** | 1.0 | **0.99997** | ✓ | 无 |
| gauss250K | 冻结基线 V5 | 401.99 | 50.360 | 0.056 | **50.416** | 92.68 | 91.44 | 1.0 | 0.89935 | ✓ | 无 |
| gauss250K | **本方案** | 401.99 | 12.807 | 0.037 | **12.844** | **127.40** | **118.92** | 1.0 | **0.99502** | ✓ | 无 |

observer 日志（基线臂 SIFT）：`[OBVSAG][cuVS] serialized GPU CAGRA snapshot(key=0x…, n=250000, dim32=128, built=true, build_us=46003163, graph_bytes=256001713)`
→ 基线建图 46.0 s，占构建窗口 93%；本方案建图 ~6.4 s，占窗口 63%，其余是表扫描 / 快照落盘 / 首查。
四个臂的日志里**都没有** `lazily built VSAG fallback`（即全程 GPU 路径，无 CPU 回退）。

复现：`docs/gpu-vector-index-hipvs-cuvs/bench/run_bench_ab.sh`（一键编译基线版与本方案版两个 `.so` 并跑同一条 SQL）。

---

## 7. 过程中的三个坑（供复核者参考）

1. **env 解析 bug**：早期版本里 `env_long()/env_double()` 在解析成功时返回默认值，
   导致所有 `SEEKDB_CUVS_*` 从未生效，一度得出"调参对建图时间毫无影响"的错误结论。
   修复后参数立刻生效（niter=7 → 建图 12.5 s）。现在所有旋钮默认即最终配置，
   环境变量只用于复现实验。**本报告里所有数字都是在参数真实生效后重测的。**
2. **cuVS itopk 硬限制（静默错误）**：`itopk > 512` 配 SINGLE_CTA 时，`RAFT_EXPECTS` 在 release 构建里被编掉，
   **不抛异常、静默返回错误邻居**（Recall@100 0.90 → 0.29，但每查询仍返回 k 个互不重复 ID）。
   同一场景下 seekdb 会因为 `lazily built VSAG fallback` 而回退到 CPU(VSAG) 索引，
   表现为吞吐从 135 掉到 49、召回从 0.99997 掉到 0.9498。
   最终版按 kernel 夹逼 itopk（SINGLE_CTA ≤ 512、AUTO ≤ 1024），MULTI_CTA 不夹逼。
3. **枚举踩坑**：`cuvsCagraSearchAlgo` 的 0/1/2/3 依次是 SINGLE_CTA / MULTI_CTA / MULTI_KERNEL / AUTO；
   早期把 `ALGO=1` 当成 SINGLE_CTA，导致"SINGLE_CTA 快 6.4×"的结论作废（实际 SINGLE_CTA 反而最慢）。
   本报告 §4.3 的表格已用正确枚举重测。

---

## 8. 复现方法

```bash
# 1) 数据准备（SIFT1M fvecs -> 250K×128 base + 4096 query + 暴力真值）
python3 docs/gpu-vector-index-hipvs-cuvs/bench/prep_data.py <sift_dir> <out_dir>

# 2) 一键 A/B：同一台机器、同一份数据、同一条 SQL，分别跑冻结基线 bridge 与本方案 bridge
docs/gpu-vector-index-hipvs-cuvs/bench/run_bench_ab.sh /workspace/ab sift250k 128 4096

# 3) 单独扫参（bridge 级，秒级）
python3 docs/gpu-vector-index-hipvs-cuvs/bench/sweep_bridge.py <configs.json>

# 4) 官方 DDL 持久化冒烟（等价原生版，评测机无 docker）
bash docs/gpu-vector-index-hipvs-cuvs/validation/run_ddl_persistence_native.sh <observer> <bridge> <work_dir>
```

环境：AMD Radeon PRO W7900D（gfx1100, 48 GB）、ROCm 7.2.4、hipVS `f96a663`、CMake 3.31.10、GCC 13.3、Rust 1.97.1。
本次实测机的 CPU 被 cgroup 限制为 16 核（评测机 24 核），因此**绝对建图时间比官方基线慢**
（官方 SIFT 中位 23 s，本机基线 49.1 s），但**比值可迁移**：
查询吞吐与官方基线几乎一致（本机 138.3/130.2 vs 官方 147/143）。

---

## 9. 风险与限制

* `persistent` 搜索模式在本实现上有召回崩塌与挂死风险，已禁用。
* 自定义 IVF-PQ 建图参数会 double-free（cuVS 会释放我们挂进去的 params），已移除该分支。
* 评测机上 hipVS 只读、无 raft/rmm 头文件 → 无法在仓内重编 cuVS 内部，本方案因此只调参数与调用方式。
* 全部改动限于桥接层；若评测机 CPU 核数更多（24 核 vs 本机 16 核），基线与本方案都会更快，
  但 GNND 主机侧开销随核数摊薄，**本方案的相对优势可能略小于本机实测的比值**。
* gauss250K 是"Hidden250K 的替身"（随机高斯，基线 Recall@100 = 0.899，与官方 Hidden 的 0.914 同量级），
  用于在无隐藏数据的前提下验证召回门槛；真正的 Hidden250K 需以评测结果为准。
  为此本方案在召回上留了很大余量（0.995 vs 0.899），即使 Hidden 数据更难也更安全。
