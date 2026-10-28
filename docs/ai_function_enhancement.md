# 赛题一：AI 函数增强 —— 实现说明

> 基线：`seekdb-2026-competition` 快照（2026-10-26）。
> 目标：在 seekdb SQL 引擎内补齐 AI_PROMPT / AI_EMBED / AI_COMPLETE / Ollama Provider / AI_RERANK / AI_EMBED 跨行批量 六项能力，全部在表达式层（`src/sql/engine/expr/ob_expr_ai/`）与共享服务层（`src/share/ai_service/`、`src/observer/ai_service/`）内完成，不经过应用层，不对模型名 / 输入 / 响应做硬编码。

## 一、改动文件

| 文件 | 改动要点 |
| --- | --- |
| `src/sql/engine/expr/ob_expr_ai/ob_expr_ai_prompt.cpp` | 类型规则放宽到 string/text/blob/binary 全家族；JSON 参数支持；LOB 读取；占位符解析重写 |
| `src/sql/engine/expr/ob_expr_ai/ob_expr_ai_complete.cpp` | 允许 JSON 模板作为 prompt；`NULL` config 视为无配置；去掉“args 只能是字符串”的限制 |
| `src/sql/engine/expr/ob_expr_ai/ob_expr_ai_embed.cpp` / `.h` | 单行 NULL 传播、dimension 校验、批量 eval（`eval_batch_func_`） |
| `src/sql/engine/expr/ob_expr_ai/ob_expr_ai_rerank.cpp` / `.h` | 分批 + 全局 index 映射 + 稳定排序 + 返回校验；对象数组按 `doc_key` 映射 |
| `src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp` | Ollama provider 分发；Embedding 乱序恢复；空响应校验；prompt 对象构造/替换重写 |
| `src/share/ai_service/ob_ai_service_struct.cpp` | `OLLAMA` provider 合法化；空 access key 仅对 Ollama 放行；provider 大小写不敏感但原样存储 |
| `src/observer/ai_service/ob_ai_service_proxy.cpp` | ALTER endpoint 仅改 provider 时不被 NULL access key 卡住 |

## 二、逐子题实现

### 2.1 AI_PROMPT 类型与模板增强（10 分）

- **类型规则**（`calc_result_typeN`）
  - 第 0 个参数（模板）：`ob_is_string_type()`，覆盖 `char/varchar/binary/varbinary/tinytext/text/mediumtext/longtext/tinyblob/blob/mediumblob/longblob`；其余类型（int/decimal/datetime/json/number 等）返回 `OB_ERR_INVALID_TYPE_FOR_OP`（MySQL 5083）。
  - 第 1+ 个参数：字符串家族 + JSON（`json_object` / `json_array` / 列存 JSON），其它类型 5083。
  - 所有字符串参数在 calc 阶段统一转 `ObVarcharType` + `CS_TYPE_UTF8MB4_BIN`，保证 `BINARY(20)` 的尾部 `\0` 原样参与模板。
- **求值**（`eval_ai_prompt`）
  - 用 `ObTextStringHelper::read_real_string_data()` 读取模板与字符串参数（正确处理 LOB header、`BINARY` padding、`LONGTEXT`）。
  - JSON 参数用 `ObJsonExprHelper::get_json_doc()` 取节点后直接挂到 `args` 数组，JSON 值原样输出：
    `ai_prompt('{0}/{0}/{1}', json_array(1,true), json_object('k',2))`
    → `{"args": [[1, true], {"k": 2}], "template": "{0}/{0}/{1}"}`
  - 参数为 SQL `NULL`（`NullType`）→ 5083；模板为 `NULL` → 5083；空模板 `''` → `OB_INVALID_ARGUMENT`（1210）。
  - 无参数调用 → `OB_ERR_PARAM_SIZE`（1582）。
- **模板对象**（`ObAIFuncPromptObjectUtils::construct_prompt_object`）
  - 输出键序固定 `{"args": ..., "template": ...}`；无参数时 `args` 为空数组。
- **占位符**（`replace_all_str_args_in_template`）
  - 两遍扫描：第一遍统计长度并校验，第二遍填充；`{0}{0}{1}` 这类重复占位符全部替换。
  - 非法下标（`{9}` 但只有 2 个参数）、未闭合 `{` 等 → `OB_INVALID_ARGUMENT` + `LOG_USER_ERROR`；数字串超长时做溢出保护，不会越界。
  - `{{`、`{abc}` 等非数字占位符保持原样。

### 2.2 AI_EMBED 单行语义（10 分）

- 不传 dimension：请求体不带 `dimensions`，结果按服务返回维度（Mock 为 768 维）输出 JSON 数组。
- 显式 dimension：
  - 第三参数必须是整数类型，decimal/float 直接 1210；
  - `dimension <= 0` → 1210；
  - config 里加入 `{"dimensions": N}`，响应向量长度必须等于 N，否则 1210。
- 返回校验：响应 `data` 必须是非空数组、每个元素必须是数组；`data[i].index` 存在时按 index 归位（**乱序响应不会错位**），index 重复/越界/缺失 → `OB_INVALID_DATA`。
- `content` 为 `NULL` → 结果 `NULL`（不请求、不报错）；`model` 为 `NULL` 或空串 → 1210。

### 2.3 AI_COMPLETE 请求与响应（10 分）

- OpenAI / OpenAI 兼容 Endpoint（`provider=openai`）走 `/chat/completions`，请求体 `{"model","messages":[{"role":"user","content":...}]}` + config 合并。
- 第三参数为 `NULL` 时视为“无配置”，正常发起请求（不再报 `OB_ERR_NULL_VALUE`）。
- config 支持 `temperature`、`max_tokens` 等任意 JSON 字段：`{"temperature":0.2,"max_tokens":16}` 会合并进请求体。
- prompt 可以是普通字符串，也可以是 `AI_PROMPT` 返回的 JSON 模板对象（`{"args":[...],"template":"..."}`）：先 `is_valid_prompt_object` 校验，再用 `replace_all_str_args_in_template` 渲染，JSON/嵌套参数原样展开为文本。
- 响应校验：`choices[0].message.content` 缺失 → `OB_INVALID_DATA`；内容为空串 → 同样报错（“响应非空校验”），不会返回空结果。

### 2.4 Ollama Provider（10 分）

- `CREATE AI MODEL ENDPOINT` 支持 `provider = 'ollama'`（大小写不敏感：`ollama`/`Ollama`/`OLLAMA` 均可）；`VALID_PROVIDERS` 增加 `OLLAMA`。
- 空 access key 兼容：`check_valid()` 中仅对 Ollama 放行空 `access_key`，其它 provider 仍要求非空；provider 校验提前到 access key 之前，避免先报错。
- provider 原样存储：`case_compare` 只用于比较，不修改写入的字符串，`select provider from DBA_OB_AI_MODEL_ENDPOINTS` 回显原样（`ollama` 仍是 `ollama`）。
- **原生 Completion 协议**：`/api/generate`，请求 `{"model","prompt":...}`（+ config），响应取 `$.response`；空响应 → `OB_INVALID_DATA`；无鉴权头。
- **原生 Embedding 协议**：`/api/embed`，请求 `{"model","input":[...]}`，响应取 `$.embeddings`（二维数组），按元素输出。
- `ObAIFuncClient` 允许空 header 集合（Ollama 无鉴权），不再因为 `headers.empty()` 直接 1210。
- ALTER endpoint 只改 provider 时，`update_ai_endpoint` 用空串而不是 NULL 写入 `access_key`（`access_key_str`），`select_ai_endpoint` 读回时把 NULL 视作空串，避免 `OB_ERR_NULL_VALUE`（4152）导致后续 `ai_complete` 失败。

### 2.5 AI_RERANK 结果正确性（10 分）

- 输入：
  - 字符串数组：元素必须都是字符串；
  - 对象数组 + `doc_key`：每个对象取 `doc_key` 字段作为文档文本，元素必须都是对象、字段必须都是字符串。
  - 其它形态 → 1210。
- 分批：每批 20 条，请求体 `{"model","query","documents"}` + config（`return_documents`、可选 `top_n`）。
- 全局 index 映射：`batch_result_add_base()` 把批内 `index` 加上批起点，得到全局下标；同时校验批内 index 落在 `[0, 批大小)`，越界 → `OB_INVALID_DATA`。
- 返回校验：`results` 必须是数组、元素必须是对象、`index` 必须是非负整数、`relevance_score` 必须是数值。
- 稳定排序：合并各批时按 `(score desc, index asc)` 排序（原实现同分时结果不稳定）。
- 对象数组：按全局 index 取回原对象 `clone()` 后附加 `_model_score` 字段，输出顺序与分数一致；返回数量、顺序、原始 id 映射正确。

### 2.6 AI_EMBED 跨行批量（10 分）

- `ob_expr_ai_embed` 注册 `eval_batch_func_`（`eval_ai_embed_batch`），表扫描时按 batch 求值。
- NULL 输入传播：`content` 为 NULL 的行直接产出 `NULL`，不进入请求、不占位，避免结果错位（`ai_enhance_batch` 中 id=2 → NULL）。
- 分组：按 `(dimension, model_id)` 分组后逐组解析 endpoint，避免同一批里混用不同模型/维度。
- 请求批：每 64 条输入合成一次 `input` 数组请求；响应条数必须与请求条数一致，否则 `OB_ERR_UNEXPECTED`；乱序响应按 `index` 恢复。
- 结果写回：用 `ObTextStringDatumResult` 的 `init_with_batch_idx(len, row_idx)` 精确写回原始行，`eval_flags` 标记已求值行，跳过的行不会重复计算。

## 三、验证

### 3.1 Mock AI Service

本地 Mock（`mock_ai_service.py`，监听 `127.0.0.1:8000`）覆盖 5 个端点，路由按 URL 后缀匹配（真实 URL 带 `/compatible-mode/v1` 等前缀）：

| 端点 | 行为 |
| --- | --- |
| `/v1/chat/completions` | 返回 `choices[0].message.content` |
| `/v1/chat/completions/empty` | 返回空 `content`（验证非空校验） |
| `/v1/chat/completions/badjson` | 返回非 JSON（验证异常响应） |
| `/v1/embeddings` | 返回 768 维向量，**index 倒序**（验证乱序恢复），支持 `dimensions` |
| `/rerank` | `query==doc` 记 1.0，包含记 0.5，返回 `index` + `relevance_score` |
| `/api/generate` | Ollama 原生 completion：`{"response": ...}` |
| `/api/embed` | Ollama 原生 embedding：`{"embeddings": [[...]]}` |

### 3.2 端到端用例

```bash
python3 tools/deploy/mysqltest.py run --suite ai_function \
  --test-set ai_function.ai_function_enhancement --base-dir <dir> \
  --work-dir /tmp/mt --host 127.0.0.1 --port 2897
```

覆盖：AI_PROMPT 的 JSON 参数/重复占位符/LONGTEXT 模板；AI_EMBED 默认与显式维度；AI_COMPLETE 的 NULL 配置、config 合并、AI_PROMPT 作 prompt；Ollama 端点创建/Provider 大小写/原生 complete 与 embed；RERANK 字符串数组与对象数组；AI_EMBED 跨行批量（NULL 传播、顺序与维度映射）。

### 3.3 结果

三套用例在干净构建（`build.py build seekdb`，产物 `build/bin/src/observer/seekdb`）上全部通过：

| 用例 | 结果 | 覆盖 |
| --- | --- | --- |
| `ai_function.ai_function_enhancement` | **PASS** (0.558s) | 评委会给的六类端到端主流程 |
| `ai_function.ai_prompt` | **PASS** (0.559s) | AI_PROMPT 类型/参数/聚合/错误码全量表 |
| `ai_function.ai_function_extra` | **PASS** (0.542s) | 自建异常与边界用例（见 3.4） |

`ai_prompt.test` 只改了 5 处与新语义冲突的旧期望：JSON 参数由 `--error 1235`（不支持）改为正常返回；`tinyblob/text/mediumblob/longtext` 四列由 `--error 5083` 改为正常返回。其余（含 `BINARY(20)` 的 `\u0000` padding、全部错误码）与原 `.result` 逐字节一致。

### 3.4 边界与健壮性用例（`ai_function_extra`）

| 场景 | 断言 |
| --- | --- |
| 空 completion 响应（`/chat/completions/empty`） | `ERROR HY000: unexpected http message: ...` |
| 非 JSON 响应（`/chat/completions/badjson`） | `ERROR 22032: Invalid JSON text.` |
| 维度不符（响应 16 维、请求 512） | `Incorrect arguments to ai_embed, result dimension is not equal to dimension`；请求 16 时 `json_length=16` |
| 重复 `index`（`/embeddings/dupidx`） | `unexpected http message`（不越界、不错位） |
| rerank 25 文档（跨两批） | 返回 25 条，`$[0].document='apple'`，`$[24].index=24` |
| rerank 对象数组 25 条 + `doc_key='text'` | `$[0].id='d01'`、`$[24].id='d25'`、共 25 条（全局 index 映射正确） |
| 表扫描批量 embed 56 行含 8 个 NULL | `total=56`、`count(ai_embed)=48`，NULL 行结果为 NULL，其余均为 768 维 |

## 四、兼容性

- 普通 SQL 与既有 AI 函数行为未变：`ai_prompt` 的错误码（1582/5083/1210）、`ai_complete` 的 OpenAI/DashScope/SiliconFlow/Hunyuan 分发、`ai_embed` 的维度校验、`ai_rerank` 的 `_model_score` 字段均保持原有约定。
- 所有新增解析路径都做了类型与越界校验，异常输入（空响应、非 JSON、缺字段、乱序、维度不符、占位符越界）只返回错误码，不崩溃、不错位。

## 五、交付物

| 路径 | 内容 |
| --- | --- |
| `src_changes/src/...` | 9 个改动后的完整源文件（按仓库目录结构放置，可直接覆盖） |
| `patch/ai_function_enhancement.patch` | 相对 `seekdb-2026-competition` 快照的完整 diff（含 3 处上游已存在的无关 hunk，见下） |
| `patch/ai_function_enhancement_minimal.patch` | 只含本次改动的 diff（对 `ob_ai_service_struct.cpp` / `ob_ai_service_proxy.cpp` 做了净化，便于评审） |
| `tools/deploy/mysql_test/test_suite/ai_function/t/` | `ai_function_enhancement.test`、`ai_prompt.test`、`ai_function_extra.test` |
| `tools/deploy/mysql_test/test_suite/ai_function/r/mysql/` | 对应的 `.result` 期望文件 |
| `docs/verification.md` | 构建与三套用例的原始输出证据 |

> 说明：`ai_function_enhancement.patch` 中 `ob_ai_service_struct.cpp` / `ob_ai_service_proxy.cpp` 还夹带了 3 处上游分支已有的改动（`url` 必须 `http(s)://` 前缀、两处 `LOG_INFO` 去掉 `K(params_jbase)`、`endpoint_name` 的 `ObHexEscapeSqlStr` 转义），与本题无关；`minimal` 补丁已剔除，两处文件的净改动只有 OLLAMA provider 校验与 access key 的空值安全处理。

应用方式（二选一）：

```bash
# 方式 A：整体覆盖
cp -r src_changes/src/* <seekdb>/src/

# 方式 B：打补丁（基线为 2026-competition 快照）
cd <seekdb> && git apply /path/to/patch/ai_function_enhancement_minimal.patch
```

构建与验证：

```bash
./build.py init && ./build.py build seekdb --jobs=12
python3 tools/deploy/mysqltest.py run --suite ai_function \
  --test-set ai_function.ai_function_enhancement \
  --base-dir /tmp/i1 --work-dir /tmp/mt1 --host 127.0.0.1 --port 2897
```
