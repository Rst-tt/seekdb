# 赛题一：AI函数增强

| 项目 | 说明 |
| --- | --- |
| 项目 | seekdb |
| 子题 | 6 个 |
| 总分 | 60 分 |


## 一、题目背景
seekdb 已支持 `AI_PROMPT`、`AI_COMPLETE`、`AI_EMBED`、`AI_RERANK` 及 AI Model Endpoint 管理。本题要求参赛者在现有实现上补齐类型支持、Provider 协议、结果校验和批量执行能力。

比赛不考察模型效果，也不需要调用真实模型。所有模型请求由组委会提供的 Mock AI Service 响应。

## 二、题目内容
### 2.1 通用要求
1. 基于 seekdb SQL 引擎实现，不得在应用层绕过。
2. 不得针对模型名、输入或响应硬编码。
3. 不得破坏现有 AI 函数和普通 SQL。
4. 异常输入或网络错误不得导致崩溃、结果错位或资源泄漏。

### 2.2 子题
| 子题 | 主要要求 |
| --- | --- |
| AI_PROMPT 类型与模板增强 | 支持 TEXT/LONGTEXT、JSON、嵌套 Prompt、重复和非法占位符、Unicode 与转义 |
| AI_EMBED 单行语义 | 支持单行和多行；校验正整数 dimension、向量维度及元素合法性 |
| AI_COMPLETE 请求与响应 | 支持普通 Prompt、AI_PROMPT、JSON config、长文本及异常响应 |
| Ollama Provider | 支持 Endpoint 增删改查、免 API Key、大小写兼容、非流式生成和 Embedding |
| AI_RERANK 结果正确性 | 支持字符串/对象数组、分批、全局 index 映射、稳定排序和返回校验 |
| AI_EMBED 跨行批量 | 一个请求携带多条输入；正确处理乱序响应、NULL、过滤、LIMIT、多批次和失败 |


## 三、评分标准
| 子题 | 评分重点 | 分值 |
| --- | --- | ---: |
| AI_PROMPT 类型与模板增强 | JSON Array/Object 参数 4；重复占位符替换 3；LONGTEXT/TEXT 模板与参数 3 | 10 |
| AI_EMBED 单行语义 |  默认维度 4；显式 dimension 参数 3；返回 JSON 及向量维度校验 3 | 10 |
| AI_COMPLETE 请求与响应 | OpenAI 模型及 Endpoint 配置 3；基础调用与 NULL 配置 3；temperature/max_tokens 配置 2；响应非空校验 2 | 10 |
| Ollama Provider | Endpoint 创建及 Provider 修改 3；空 access key 兼容 2；原生 Completion 协议 3；原生Embedding 协议 2 | 10 |
| AI_RERANK 结果正确性 | 字符串数组输入 3；对象数组及文本字段选择 3；返回数量、顺序与原始 ID 映射 4 | 10 |
| AI_EMBED 跨行批量 | 表扫描中的多行调用 3；NULL 输入传播 3；结果与原始行的顺序、维度映射 4 | 10 |
| **合计** |  | **60** |


## 四、测试代码
| 类型 | 文件 | 主要内容 |
| --- | --- | --- |
| Unittest | `unittest/sql/engine/expr/ob_expr_ai_prompt_test.cpp` | JSON、重复占位符、嵌套 Prompt、非法下标 |
| Unittest | `unittest/sql/engine/expr/test_ob_openai_utils.cpp` | Embedding 校验与乱序恢复、Ollama 请求/响应 |
| Mysqltest | `tools/deploy/mysql_test/test_suite/ai_function/t/ai_function_enhancement.test` | 六类功能的端到端主流程 |
| Mysqltest | `tools/deploy/mysql_test/test_suite/ai_function/t/ai_prompt.test` | AI_PROMPT 类型和参数测试 |


`ai_function_enhancement.test` 依赖运行在 `0.0.0.0:8000` 的 Mock AI Service。
