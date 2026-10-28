# 赛题三：AI列

| 项目 | 说明 |
| --- | --- |
| 项目 | seekdb |
| 评分项 | 6 个，按功能模块分项计分 |
| 总分 | 50 分 |


## 一、题目背景
seekdb 已支持 `AI_COMPLETE`、`AI_EMBED` 及 AI Model Endpoint 管理。应用可以调用 AI 函数生成摘要、提取标签或计算文本向量，但仍需自行维护结果，并在原始数据更新后处理重新计算、失败重试和并发写回。

本题要求参赛者基于已有 AI 函数能力，在数据库内核中实现 **异步 AI 列（AI COLUMN）**：用户通过 SQL 声明列的生成规则，数据库在业务事务提交后自动调用模型、存储结果，并在依赖数据变化时重新生成。核心挑战是在外部服务延迟、失败和请求乱序返回的情况下，保证事务、结果和索引的一致性。

比赛不考察模型效果。基础功能通过 Mock AI Service 验证，无需调用真实模型；故障与调用效率评测可结合配套评测网关进行。

## 二、题目内容
### 2.1 通用要求
1. 基于 seekdb SQL 引擎、Schema、DML 和后台任务机制实现，不得在应用层绕过。
2. 不得针对模型名、输入、主键或 Mock 响应硬编码，不得破坏现有 AI 函数和普通 SQL。
3. 本题限定 MySQL 模式和异步生成，支持 `sync_mode='async'`；不要求 AI 列之间的链式依赖。
4. 模型调用在业务事务提交后执行；事务回滚不得遗留任务，也不得触发该事务对应的模型请求。
5. 超时、异常响应或类型转换失败不得导致崩溃、错误覆盖、结果错位或资源泄漏。
6. 优化不得改变模型输入、跳过必要计算或放宽结果一致性；业务 SQL 不得直接赋值修改 AI 结果列。

### 2.2 使用示例
假设已注册文本模型 `text_model`、输出维度为 4 的向量模型 `embed_model` 及对应 Mock Endpoint：

```sql
CREATE TABLE docs (
  id BIGINT PRIMARY KEY,
  title VARCHAR(100),
  content VARCHAR(2000),
  tags JSON AI COLUMN (
    AI_COMPLETE('text_model', CONCAT('提取分类标签，返回 JSON：', content))
  ) WITH (sync_mode='async', max_retries=2),
  emb VECTOR(4) AI COLUMN (
    AI_EMBED('embed_model', content)
  ) WITH (sync_mode='async', max_retries=2)
);

INSERT INTO docs(id, title, content)
VALUES (1, '示例文档', '数据库支持自动生成文本标签和检索向量。');

-- 默认模式下，尚未完成的结果为 NULL；后台完成后可直接读取。
SELECT id, tags, emb FROM docs;

-- 更新依赖列，旧结果失效并重新生成。
UPDATE docs SET content = '更新后的正文' WHERE id = 1;

-- 更新无关列，不重新生成。
UPDATE docs SET title = '新的标题' WHERE id = 1;

-- 对指定 AI 列按状态或业务条件重新生成。
ALTER TABLE docs RETRY AI COLUMN tags WHERE status = 'FAILED';
ALTER TABLE docs RETRY AI COLUMN tags WHERE id = 1;
```

### 2.3 子题
| 子题 | 主要要求 |
| --- | --- |
| AI 列 DDL 与类型约束 | 支持建表、加列和查看定义；保存表达式、依赖及配置；校验函数、参数和向量维度；根据结果类型补充提示词并校验写入结果 |
| DML 联动与事务一致性 | 插入、依赖列更新和主键更新触发任务；无关列更新不触发；数据变更、结果失效和任务创建同事务；支持批量操作、回滚和删除清理 |
| 异步调度与结果写回 | 持久化任务、并发领取、执行中断后的恢复；校验请求标识、定义版本和任务归属；防止迟到结果覆盖；同一行同次变更的 AI 列分组提交 |
| 条件重试与严格读取 | 支持有限自动重试、最终失败状态和按条件手动重试；区分 PENDING、READY、FAILED；实现 `ai_column_strict_read` |
| AI 向量列与索引维护 | 生成向量并参与索引检索；失效时移除旧向量，写回后加入新向量；保证更新和回滚时数据与索引一致 |


### 2.4 关键语义
+ **默认读取：** 未完成及最终失败的 AI 结果为 `NULL`，通过状态区分原因；不能仅以结果是否为 `NULL` 判定任务是否完成。
+ **严格读取：** `SET SESSION ai_column_strict_read = ON` 后，语句引用的 AI 列中存在未就绪行即报错。采用列级检查，即使查询条件只选中就绪行也不例外；仅访问普通列或其他全部就绪的 AI 列不受影响。
+ **按条件重试：** 仅影响指定 AI 列和命中的行，可重新生成 READY 行。`status` 是重试语句中的状态伪列；反引号引用的 `status` 表示同名业务列。
+ **分组发布：** 同一行同次变更触发的一组 AI 列在处理达到终态后，以一个事务发布结果和状态。成功列为 READY，最终失败列为 FAILED；不要求所有模型调用都成功。
+ **旧结果保护：** 在“旧正文请求较慢、新正文已提交”的情况下，迟到结果不能覆盖新数据；删除行后也不能被后台写回恢复。

## 三、评分标准
以下为建议分值分配。各项按对应测试点计分，性能比较须先满足对应功能的正确性要求。

| 子题 | 评分重点 | 分值 |
| --- | --- | ---: |
| AI 列 DDL 与类型约束 | CREATE/ALTER 语法与系统表元数据 3；隐藏状态列、结果列及配置持久化 2；向量维度、AI 列依赖等非法定义校验 3 | 8 |
| DML 联动与事务一致性 | 插入及批量任务生成 2；依赖列、无关列及主键更新 3；事务提交与回滚一致性 3；删除行和待执行任务清理 2 | 10 |
| 异步调度与结果写回 | 多 AI 列请求分组及类型化结果 3；异步执行和结果原子发布 3；自动重试、终态失败及任务清理 3；最新请求胜出和旧请求结果防覆盖 3 | 12 |
| 失败重试与严格读取 | 按状态、主键及业务字段选择重试行 3；Strict Read 引用列范围及列级完整性 3；SELECT、UPDATE、DELETE 错误语义 2；READY/FAILED、零行命中及非法 RETRY 语法 2 | 10 |
| 条件重试与严格读取 | Embedding 生成、HNSW/VSAG 索引及 ANN 查询 3；依赖更新后向量失效及旧索引项移除 2；向量重新生成和索引同步 2；事务回滚、任务清理及索引可见性恢复 3 | 10 |
| **合计** |  | **50** |


性能评测使用统一的资源、模型响应、服务延迟和并发上限。统计业务写入延迟、从事务提交到结果可见的完成延迟、任务完成吞吐及实际模型调用次数，不以任务入队速度代替完成性能。

故障场景包括超时、HTTP 429/5xx、连接中断，以及“上游已完成但响应丢失”。最后一种情况可能导致重复推理，应统计实际到达上游的调用。若开展真实模型扩展评测，Token 用量和估算费用按统一配置统计，不作为模型效果评价。

## 四、测试代码
下表为本次新增代码中的测试入口及现有覆盖内容；中断恢复、跨端点同 URL 限流和性能计分场景仍需配套专项评测。

| 类型 | 文件 | 主要内容 |
| --- | --- | --- |
| Unittest | `unittest/sql/parser/test_ai_column_parser.cpp` | CREATE/ADD/MODIFY 语法和 SHOW AI COLUMNS |
| Unittest | `unittest/sql/resolver/test_ai_column_resolver.cpp` | 选项、依赖限制、表达式、向量维度及类型提示词 |
| Unittest | `unittest/share/schema/test_ai_column_schema.cpp` | 元数据、拷贝与序列化、列 ID 重映射和隐藏列 |
| Mysqltest | `tools/deploy/mysql_test/test_suite/ai_function/t/ai_column_ddl.test` | 建表、加列、元数据、维度及依赖校验 |
| Mysqltest | `tools/deploy/mysql_test/test_suite/ai_function/t/ai_column_t1_dml.test` | 任务创建、依赖失效、无关更新、主键变更、批量、回滚与删除 |
| Mysqltest | `tools/deploy/mysql_test/test_suite/ai_function/t/ai_column_worker.test` | 类型提示词、分组可见性、自动重试、最终失败及迟到结果保护 |
| Mysqltest | `tools/deploy/mysql_test/test_suite/ai_function/t/ai_column_retry_strict.test` | 按状态和业务条件重试、状态伪列、严格读取 |
| Mysqltest | `tools/deploy/mysql_test/test_suite/ai_function/t/ai_column_vector_index.test` | 向量生成、VSAG 索引、ANN 查询、失效、重算与回滚 |
| Mock 服务 | `tools/deploy/mysql_test/test_suite/ai_function/include/ai_column_mock.py` | 确定性文本/向量响应、请求断言和故障模拟 |
| 评测网关 | `tools/ai_gateway/README.md`、`tools/ai_gateway/test_gateway.py` | 透明代理、延迟与故障注入、响应丢失、用量及费用统计 |


上述 Mysqltest 按各自脚本启动或使用本地 Mock 服务，端口为 19120～19124；DDL 用例仅定义 Endpoint，不发起模型请求。评测网关是独立工具，不属于数据库事务边界，其单元测试使用 Fake Upstream，无需真实模型凭据。
