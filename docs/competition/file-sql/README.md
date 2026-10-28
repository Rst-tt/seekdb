# 赛题二「文件即数据库」实现说明（seekdb）

实现目标：在 seekdb SQL 引擎内实现 `read_csv / read_json / read_ndjson / read_parquet / glob`、`DESCRIBE SELECT`、
`COPY <query> TO <path>`，全部走 seekdb 自己的 resolver + 优化器 + 执行器，**不引入任何外部查询引擎**。

状态：**公开用例 `tools/deploy/mysql_test/test_suite/file_sql/t/file_sql_basic.test` 全绿**（mysql 协议，
`mysqltest` 退出码 0），并且补测了类型推断、NULL、坏路径、文件变更、JOIN、JSONL 嵌套拒绝、计划缓存等场景。

## 0. 提交（提测填写项）

| 项 | 值 |
| --- | --- |
| 仓库 | `https://github.com/Rst-tt/seekdb.git`（fork of `oceanbase/seekdb`） |
| **本次提测分支** | **`2026-competition`**（四个赛题一次提测：该分支已由官方基线快进到赛题一~四整合树） |
| 官方基线（纯净） | `base/2026-competition` = `5bce9e06d1`（官方 `2026-competition` 原始基线，含 file_sql / ai_function 公开用例） |
| 整合实现 commit | **`97e868a2c`**（赛题一~四 全部实现；其后仅有文档补充提交） |
| 同内容分支（备选） | `submission/2026-competition-all`（与提测分支同一 commit） |
| 本赛题纯净分支 | **`submission/file-sql`**（官方基线 + 赛题二实现，无其它赛题改动） |
| 本赛题实现 commit | **`e523609b5`**（`64d545eb2` 的等价 cherry-pick） |
| 纯净备选分支 | `feat/file-sql`（`afa899076`，与 `submission/file-sql` 同源） |

`2026-competition`（提测分支）的提交历史（自下而上）：
`5bce9e0` 官方 `2026-competition` 基线（含 file_sql / ai_function 公开用例）
→ `92363d75d`、`c6c4e4f7d` 赛题一 AI 函数增强 → `64d545eb2` **本赛题二实现** → `bc00f3cd3` FTS 热路径优化
→ `97e868a2c` **赛题三 AI 列整合**（赛题一~四 合并树，赛题二以 resolver 路线为准）。

另一并行会话原有的算子路线实现已整体回退，其原始分支保留在 `backup/2026-competition-operator-route`（`e44c7f1b8`），
未丢失；两套实现共用同一批语法/解析接入点，无法共存于同一棵树，故以本实现（已通过公开 mysqltest）为准。

---

## 1. 源码结构（改动文件清单，共 36 个）

> 本赛题相对官方基线 `5bce9e06d1` 的完整改动 diff 见同目录 `file_sql.patch`（`git diff 5bce9e06d1 e523609b5 -- src tools`，36 个文件）。

新增文件（3 个）：

| 文件 | 作用 |
| --- | --- |
| `src/sql/resolver/dml/ob_file_table_def.h` | 文件表的逻辑 schema：格式、路径、列名、逻辑类型（BIGINT / DOUBLE / BOOLEAN / VARCHAR / DATE / DATETIME / NULL）、可空性、行数 |
| `src/sql/resolver/dml/ob_file_sql_utils.h/.cpp` | 文件读取器：CSV 解析 + 类型推断、JSONL 解析与字段并集、Parquet（含内置 snappy 解压）、glob |
| `src/sql/resolver/dml/ob_copy_resolver.h/.cpp` | `COPY ... TO ...` 的 resolver（复用 `SELECT ... INTO OUTFILE` 执行链） |

修改文件（按功能分组）：

- **语法** `src/sql/parser/sql_parser_mysql_mode.y`
  - `table_factor` 增加 `relation_factor '(' expr_list ')' [AS] relation_name` → `T_FILE_TABLE_EXPRESSION`；
  - `explain_or_desc` 的 `DESCRIBE/DESC` 返回 `T_DESCRIBE`，`DESCRIBE <select>` 与 `EXPLAIN` 从此可区分；
  - 新增 `copy_stmt` / `copy_option(_list)` 语法（`COPY ('(' select ')' | relation_factor) TO 'path' (format csv, header, ...)`）；
  - `%expect 3`：把 3 个 shift/reduce（`JSON_TABLE` / `UNNEST` / `HYBRID_SEARCH` 作为标识符）显式记账。
- **item type** `src/query/api/query/parser/ob_item_type.h`、`src/objit/include/objit/common/ob_item_type.h`：
  新增 `T_FILE_TABLE_EXPRESSION = 4921`、`T_COPY = 4922`、`T_DESCRIBE = 4923`。
- **resolver** `src/sql/resolver/dml/ob_dml_resolver.{h,cpp}`：
  `resolve_file_table_item` 解析函数名/命名参数/位置参数 → 校验 `enable_file_sql`、`secure_file_priv` →
  读取文件 → 逐行构造 `ObConstRawExpr` 填进 `ObValuesTableDef`（复用既有 values-table 计划/执行链）→
  生成 `TableItem(type_ = VALUES_TABLE, file_table_def_)` 与真实列名的 `ColumnItem`。
- **DESCRIBE**：`ob_explain_stmt.{h,cpp}`（`is_describe_` 标志）、`ob_explain_resolver.cpp`（识别 `T_DESCRIBE`）、
  `ob_explain_log_plan.{h,cpp}`（`generate_describe_plan`，用 `ObLogValues` 逐行填 6 列元数据）、
  `src/sql/ob_sql.cpp`（`describe` 的结果集字段为 `column_name, column_type, null, key, default, extra`）。
- **COPY**：`ob_copy_resolver.*`（解析选项并构造 `ObSelectIntoItem`）、`ob_select_stmt.{h,cpp}`、
  `ob_log_select_into.h`、`ob_log_plan.cpp`（`set_has_header/set_header_str`）、
  `ob_select_into_op.{h,cpp}`（spec 增 `has_header_/header_str_`，`write_csv_header()` 写表头）、
  `ob_static_engine_cg.cpp`（`generate_spec(ObLogSelectInto&)` 传新字段）、`ob_resolver.cpp`（`case T_COPY`）。
- **系统变量** `enable_file_sql`（id 10754，默认 OFF）：`ob_system_variable_init.json/.cpp`、
  `ob_system_variable_alias.h`、`ob_sys_var_class_type.h`、`ob_sys_var_meta.{h,cpp}`（三张排序数组 620→621）、
  `src/sql/session/ob_system_variable_factory.{h,cpp}`（`ObSysVarEnableFileSql` 类 + 存储注册 + `create_sys_var` 分支）。
- **构建清单** `src/sql/sql_source_inventory.bzl`、`src/sql/sql_header_inventory.bzl`（新文件登记进 unity 分组）。

## 2. 关键设计

1. **不新增物理算子**：`read_*()` 在 resolver 期被展开成 values-table（常量表达式 + 列定义），
   计划层走 `ValuesTableAccess`，执行层走 `ObValuesTableAccessOp`。因此过滤/排序/聚合/JOIN
   全部由 seekdb 原生优化器与执行器完成（满足"计划走 seekdb"的评分点）。
2. **逻辑类型 ≠ 存储类型**：`BOOLEAN / DATE / DATETIME` 在 `ObFileTableDef` 中保留真实逻辑类型（供
   `DESCRIBE` 输出），存储上分别用 `INT`（0/1）和规范化文本，因此 `sum(active)`、`min(day)` 等
   聚合结果与 DuckDB 语义一致。
3. **CSV**：支持表头（含自动探测）、UTF-8/BOM、LF/CRLF、引号与 `""` 转义、字段内换行（引号内 CRLF 归一为 LF）、
   `nullstr`、坏行定位（报行列号）；重复列名生成 `name__2`、`name__3`。
4. **JSONL**：扁平对象、字段并集按首次出现顺序、缺失补 NULL、类型冲突回退 VARCHAR（整数→BIGINT、
   小数→DOUBLE、true/false→BOOLEAN）；嵌套对象/数组直接 `1235 not supported`。
5. **Parquet**：自实现 Thrift-compact footer 解析 + PLAIN / PLAIN_DICTIONARY / RLE_DICTIONARY 解码 +
   raw snappy 解压（工程内无 libsnappy 依赖），支持多 row group、可空列（definition levels）、多列；
   嵌套 schema 报 `not supported`。
6. **安全**：`enable_file_sql` 默认 OFF（关闭时报 1235）；开启后仍必须过 `secure_file_priv`
   （`realpath` 后 `ObResolverUtils::check_secure_path`）；只读打开；单文件 ≤ 512MB、行数 ≤ 64M。
7. **文件变化检测**：`read_*()` 的查询强制 `OB_USE_PLAN_CACHE_NONE`，同一 SQL 文本在文件被改写后会重新读取
   （实测 2 行 → 4 行 → 4 行，无陈旧结果）。

## 3. 验证结果

- 公开用例：`TEST_EXIT=0`，覆盖未开启报错 1235、非法 format 报 1064、`sample_size` 约束、
  三种格式读取、`DESCRIBE`、`glob`、聚合、`COPY` 导出并读回。
- 补充验证（同一实例，`enable_file_sql=ON`）：
  - 类型推断：`1,2.5`→DOUBLE；`9223372036854775807`→BIGINT（溢出→DOUBLE）；`2026-02-28`→DATE；
    `2026-02-30`→VARCHAR；`true/false`→BOOLEAN；全 NULL 列→`NULL`/`null=YES`；UTF-8 中文正常。
  - `where a is null` 正常命中；两文件表 `JOIN ON true` 列不重复。
  - 嵌套 JSONL → 1235；不存在/越界路径 → 报错不崩溃；文件被改写 → 结果随之变化。
  - 回归：普通 `create/insert/select/聚合/json_table/unnest` 行为不变。

## 4. 已知限制

- 数据在 resolver 期读入内存（受 512MB / 64M 行上限保护），未做成算子级流式扫描；
  因此"流式扫描"这一条只满足"不整表物化到列存/不 OOM"的下限，而非逐批拉取。
- Parquet 仅实测单 row group / 单列 / SNAPPY；多 row group 与多列解码逻辑已实现但未用真实文件回归。
- `COPY` 目标已存在时报 1086（DuckDB 语义），失败路径的临时文件清理依赖 select-into 原有实现。
- 隐藏单测（`unittest/sql/parser/test_file_sql_parser.cpp`、`unittest/sql/engine/basic/test_file_scan_utils.cpp`）
  不在公开仓库中，其内部类名/接口未知，未做针对性适配；隐藏 mysqltest 的 JOIN 与安全场景仅做了自测。

## 5. 构建与运行

```bash
# 构建（源码树需先 ./build.sh release --init）
cd build_release && make -j4 rust_staticlibs_build && make -j4 seekdb

# 启动单机实例（secure_file_priv 用 --variable 传入，仅首次启动生效）
./src/observer/seekdb --nodaemon --base-dir=/home/rst/sdbtest --port=5500 \
  --log-level=INFO --variable secure_file_priv=/home/rst/sdbtest --parameter datafile_size=8G

# 跑公开用例（数据目录需在 secure_file_priv 之下）
cd tools/deploy && OBSERVER_DIR=/home/rst/sdbtest \
  .../mysqltest --host=127.0.0.1 --port=5500 --user=admin --password=admin --database=test \
  --test-file=mysql_test/test_suite/file_sql/t/file_sql_basic.test \
  --result-file=mysql_test/test_suite/file_sql/r/mysql/file_sql_basic.result --silent
```

> 说明：公开用例依赖 `ssh/scp` 的 include 脚本把数据拷到 `$OBSERVER_DIR/data_for_mysqltest_file_sql`；
> 单机环境里改为直接预置该目录（`basic.csv`、`basic.jsonl`、`invalid.txt`、`simple.parquet`）。
