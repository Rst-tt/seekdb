# 验证证据（AI 函数增强）

环境：Ubuntu 22.04（WSL2），24 核，seekdb `2026-competition` 快照，bazel 9.3.0。
Mock AI Service：`tools/mock/mock_ai_service.py`，监听 `127.0.0.1:8000`，路由按 URL 后缀匹配。

## 1. 构建

```text
$ ./build.py init
$ ./build.py build seekdb --jobs=12
INFO: Elapsed time: 3.042s, Critical Path: 2.64s
INFO: Build completed successfully, 5 total actions

$ ls -l build/bin/src/observer/seekdb
-rwxr-xr-x 1 rst rst 1317410896 Oct 28 11:17 build/bin/src/observer/seekdb
```

## 2. 三套用例（同一枚二进制）

```text
$ python3 tools/deploy/mysqltest.py run --suite ai_function \
    --test-set ai_function.ai_function_enhancement --base-dir <dir> --work-dir /tmp/f1 --port 2895
[       OK ] ai_function.ai_function_enhancement (0.558s)
slice 0 finished: cases=1, failed=0, success=True
{"case_count": 1, "cases": ["ai_function.ai_function_enhancement"], "error": null,
 "failed_cases": [], "slice_count": 1, "slice_index": 0, "success": true}

[       OK ] ai_function.ai_prompt (0.559s)
slice 0 finished: cases=1, failed=0, success=True
{"case_count": 1, "cases": ["ai_function.ai_prompt"], "error": null,
 "failed_cases": [], "slice_count": 1, "slice_index": 0, "success": true}

[       OK ] ai_function.ai_function_extra (0.542s)
slice 0 finished: cases=1, failed=0, success=True
{"case_count": 1, "cases": ["ai_function.ai_function_extra"], "error": null,
 "failed_cases": [], "slice_count": 1, "slice_index": 0, "success": true}
```

## 3. `ai_function_enhancement` 关键断言（节选实际输出）

| 语句 | 实际结果 |
| --- | --- |
| `ai_prompt('{0}/{0}/{1}', json_array(1,true), json_object('k',2))` | `{"args": [[1, true], {"k": 2}], "template": "{0}/{0}/{1}"}` |
| `ai_prompt(t, a)`（t/a 为 longtext） | `{"args": ["lob"], "template": "{0}-ok"}` |
| `ai_complete('enhance_complete','hello',null)` | `complete_ok = 1` |
| `ai_complete(m, ai_prompt('describe {0}', json_object('text','你好')), '{"temperature":0.2,"max_tokens":16}')` | `complete_config_ok = 1` |
| `json_length(ai_embed('enhance_embed','single-row'))` | `768` |
| `json_length(ai_embed('enhance_embed','dimension-row',768))` | `768` |
| 批量：`select id, json_length(ai_embed('enhance_embed',content)) from ai_enhance_batch order by id` | `1→768, 2→NULL, 3→768` |
| `json_length(ai_rerank('enhance_rerank','apple','["apple","banana","fruit","vegetable"]',null))` | `4` |
| `json_extract(r,'$[0].id')`（对象数组 + `doc_key='text'`） | `'a'`，`json_length = 2` |
| `create endpoint provider='ollama' access_key=''` | 成功；`select provider` 回显 `ollama` |
| `alter endpoint ... '{"provider":"Ollama"}'` | `select provider` 回显 `Ollama`，`ai_complete` 前后均返回 `1` |
| `length(ai_complete('enhance_ollama_complete','hello'))` | `1` |
| `json_valid(ai_embed('enhance_ollama_embed','hello'))` | `1` |

## 4. `ai_function_extra`（边界用例，实际记录）

```text
-- 空 completion 响应
ERROR HY000: unexpected http message: {"id": "cmpl-empty", ..., "content": "" ...}
-- 非 JSON 响应
ERROR 22032: Invalid JSON text.
-- 维度不符（请求 512，响应 16 维）
ERROR HY000: Incorrect arguments to ai_embed, result dimension is not equal to dimension
-- 重复 index
ERROR HY000: unexpected http message: {"data": [{"index": 0, ...}, {"index": 0, ...}]}
-- rerank 25 文档跨两批
n25 = 25        first_doc = apple      $[24].index = 24
-- rerank 对象数组 25 条 + doc_key
first_id = d01  n25_obj = 25           last_id = d25
-- 表扫描批量 embed（56 行，8 行 NULL）
total = 56      non_null = 48          id 11/13 → NULL，其余 → 768
```

## 5. 备注

- 上述用例的 Mock 端点 URL 带 `/compatible-mode/v1` 等前缀，Mock 采用后缀匹配。
- `ai_function_enhancement.test` 依赖运行在 `0.0.0.0:8000` 的 Mock AI Service（本地以 `127.0.0.1:8000` 复现）。
- 二进制中的 `trace1` 字符串是 Rust 侧符号名，不是本实现的调试日志（调试日志已在构建前移除）。
