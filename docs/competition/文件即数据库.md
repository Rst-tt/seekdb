# 赛题二：文件即数据库

| 项目 | 说明 |
| --- | --- |
| 项目 | seekdb |
| 评分项 | 8 项，按功能逐项计分 |
| 总分 | 50 分 |


## 一、题目背景
AI Agent 经常需要分析本地 CSV、JSONL、Parquet 文件。传统流程需要先建表、导入数据，再执行 SQL，增加了准备工作和数据副本。

本题要求在 seekdb 中实现“文件即数据库”：通过 `read_csv`、`read_json` / `read_ndjson`、`read_parquet` 直接读取文件，自动生成列结构，并由 seekdb SQL 引擎完成过滤、聚合、排序和 JOIN。用户无需预先建表或导入数据。

## 二、题目内容
### 2.1 通用要求
1. 基于 seekdb SQL 引擎实现，不得使用 DuckDB、SQLite、Polars SQL 等外部查询引擎代执行。
2. 不得针对文件名、数据内容或查询结果硬编码。
3. 面向单进程本地文件场景，输入文件只读；功能默认关闭，开启 `enable_file_sql` 后仍须通过 `secure_file_priv` 校验。
4. 不得破坏现有普通 SQL；异常数据、路径越界和文件变化不得导致崩溃、错误结果或资源泄漏。
5. 文件扫描应流式执行，支持批量取数和重扫，不得将全文件数据物化到内存。

### 2.2 子题
| 子题 | 主要要求 |
| --- | --- |
| SQL 接口与文件发现 | 支持 `read_csv/read_json/read_parquet`、`glob`、`DESCRIBE SELECT * FROM read_*()`；命名参数、别名和动态列元数据正确 |
| CSV 读取 | 支持表头、UTF-8、LF/CRLF、引号与转义、字段内换行、NULL、坏行诊断和批量扫描 |
| JSONL 读取 | 支持扁平对象、字段并集、缺失字段补 NULL、类型冲突处理；拒绝嵌套对象和数组 |
| Parquet 读取 | 支持 Footer Schema、多 Row Group、可空列、基础类型及列裁剪 |
| 确定性类型推断 | 支持 BIGINT、DOUBLE、BOOLEAN、VARCHAR、DATE、DATETIME、NULL；处理类型宽化、边界值和全 NULL 列 |
| 单文件 SQL | 支持表达式、别名、WHERE、ORDER BY、LIMIT、GROUP BY、HAVING 及 COUNT/SUM/AVG/MIN/MAX |
| 双文件 JOIN | 支持 INNER JOIN、LEFT JOIN 和跨格式 JOIN，使用 seekdb 原生执行计划 |
| 安全、稳定性与工程质量 | 功能开关、路径限制、文件变化检测、资源上限、错误诊断和单元测试 |


CSV / JSONL 使用全量 Schema 推断，`sample_size` 仅支持 `-1`。本题不要求远程文件、多文件自动合并、嵌套结构或分布式扫描。`glob()` 仅发现文件，返回一列 `file`。

同时支持 `COPY table-or-query TO path` 导出 CSV，包括表头、分隔符、引号、转义和 NULL 文本选项。输出可由 `read_csv` 读回；目标已存在时失败，查询失败时清理临时文件。COPY 不单列分值，评分总分沿用原设计的 30 分。

## 三、评分标准
| 子题 | 评分重点 | 分值 |
| --- | --- | ---: |
| SQL 接口与文件发现 | 读取入口、glob、DESCRIBE；命名参数、别名和动态列元数据 | 7 |
| CSV 读取 | 表头与 UTF-8；引号、转义与换行；NULL、坏行诊断及批量扫描 | 10 |
| JSONL 读取 | 扁平对象；字段并集与缺失值；NULL、类型冲突及错误定位 | 7 |
| Parquet 读取 | Footer Schema；多 Row Group；可空列、基础类型和列裁剪 | 6 |
| 确定性类型推断 | 七类类型、宽化规则、数值边界、日期合法性及全 NULL 列 | 6 |
| 单文件 SQL | 投影与表达式、过滤、排序与 LIMIT、分组与聚合；计划走 seekdb | 6 |
| 双文件 JOIN | INNER JOIN、LEFT JOIN、至少一种跨格式 JOIN；计划走 seekdb | 4 |
| 安全、稳定性与工程质量 | 默认关闭、secure_file_priv、防路径逃逸、文件变化、资源限制及测试 | 4 |
| **合计** |  | **50** |


各项按公开与隐藏测试的通过比例计分。使用外部查询引擎代执行，整题 0 分；突破只读边界或绕过 `secure_file_priv`，安全项 0 分并作为严重缺陷处理。不单独进行性能排名，因全文件物化导致 OOM 的用例判定失败。

## 四、测试代码
| 类型 | 文件 | 主要内容 |
| --- | --- | --- |
| Unittest | `unittest/sql/parser/test_file_sql_parser.cpp` | 文件读取函数、别名、命名参数、glob、DESCRIBE 和 COPY 语法 |
| Unittest | `unittest/sql/engine/basic/test_file_scan_utils.cpp` | CSV 推断与读取选项、JSONL 字段并集、文件变化检测、Parquet 编码与 Snappy |
| Mysqltest | `tools/deploy/mysql_test/test_suite/file_sql/t/file_sql_basic.test` | 功能开关、三种格式读取、聚合、Schema 查看、文件发现及 COPY 导出读回 |


测试数据位于 `tools/deploy/mysql_test/test_suite/file_sql/data/`，由配套 include 脚本部署和清理。运行文件查询前需开启 `enable_file_sql`，并将测试目录配置在 `secure_file_priv` 允许范围内。当前基础测试未覆盖全部评分用例，JOIN、安全和异常路径等需补充专项测试。
