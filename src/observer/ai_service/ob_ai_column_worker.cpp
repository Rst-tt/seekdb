/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SERVER
#include "ob_ai_column_worker.h"

#include <unistd.h>
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "lib/allocator/page_arena.h"
#include "lib/string/ob_sql_string.h"
#include "share/ob_server_struct.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "observer/omt/ob_ai_service.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace observer
{

namespace
{
constexpr int64_t AI_COLUMN_STATUS_PENDING = 0;
constexpr int64_t AI_COLUMN_STATUS_READY = 1;
constexpr int64_t AI_COLUMN_STATUS_FAILED = 2;
constexpr int64_t AI_COLUMN_WORKER_MAX_ROWS_PER_TABLE = 32;
constexpr int64_t AI_COLUMN_WORKER_IDLE_SLEEP_MS = 100;

int append_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("`"))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
      if (name[i] == '`') {
        ret = sql.append("``", 2);
      } else {
        ret = sql.append(name.ptr() + i, 1);
      }
    }
    if (OB_SUCC(ret)) {
      ret = sql.append("`");
    }
  }
  return ret;
}

int append_qualified_table(ObSqlString &sql, const ObString &db_name, const ObString &table_name)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(append_identifier(sql, db_name))) {
  } else if (OB_FAIL(sql.append("."))) {
  } else if (OB_FAIL(append_identifier(sql, table_name))) {
  }
  return ret;
}

int obj_to_sql_literal(ObObj &obj, ObSqlString &sql, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  if (obj.is_null()) {
    ret = sql.append("NULL");
  } else {
    const int64_t buf_len = 1024 * 1024;
    char *buf = static_cast<char *>(allocator.alloc(buf_len));
    int64_t pos = 0;
    ObObjPrintParams print_params;
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(obj.print_sql_literal(buf, buf_len, pos, print_params))) {
      if (OB_SIZE_OVERFLOW == ret) {
        ret = OB_BUF_NOT_ENOUGH;
      }
    } else if (OB_FAIL(sql.append(buf, pos))) {
    }
  }
  return ret;
}

int evaluate_ai_column_value(const ObTableSchema &table_schema,
                             const ObColumnSchemaV2 &ai_column,
                             const ObString &db_name,
                             const ObString &table_name,
                             const ObSqlString &pk_where,
                             ObIAllocator &allocator,
                             bool &evaluated,
                             ObSqlString &value_literal)
{
  int ret = OB_SUCCESS;
  evaluated = false;
  const ObString embed_model = ai_column.get_ai_column_embed_model();
  const ObString embed_input = ai_column.get_ai_column_embed_input();
  const ObString complete_model = ai_column.get_ai_column_complete_model();
  const ObString complete_prompt = ai_column.get_ai_column_complete_prompt();
  if (!complete_model.empty() && !complete_prompt.empty()) {
    ObSqlString prompt_query;
    ObString prompt_text;
    if (OB_FAIL(prompt_query.append("SELECT ("))) {
    } else if (OB_FAIL(prompt_query.append(complete_prompt.ptr(), complete_prompt.length()))) {
    } else if (OB_FAIL(prompt_query.append(") FROM "))) {
    } else if (OB_FAIL(append_qualified_table(prompt_query, db_name, table_name))) {
    } else if (OB_FAIL(prompt_query.append(" WHERE "))) {
    } else if (OB_FAIL(prompt_query.append(pk_where.ptr()))) {
    }
    if (OB_SUCC(ret)) {
      SMART_VAR(ObMySQLProxy::MySQLResult, prompt_res) {
        ObObj prompt_obj;
        if (OB_FAIL(GCTX.sql_proxy_->read(prompt_res, prompt_query.ptr()))) {
        } else if (OB_ISNULL(prompt_res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_ITER_END == (ret = prompt_res.get_result()->next())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(ret)) {
        } else if (OB_FAIL(prompt_res.get_result()->get_obj(static_cast<int64_t>(0), prompt_obj))) {
        } else if (prompt_obj.is_null()) {
          ret = value_literal.append("NULL");
          evaluated = OB_SUCC(ret);
        } else {
          prompt_text = prompt_obj.get_string();
          if (prompt_text.empty()) {
            ret = value_literal.append("NULL");
            evaluated = OB_SUCC(ret);
          }
        }
      }
    }
    if (OB_SUCC(ret) && !evaluated) {
      ObAIFuncExprInfo *info = NULL;
      const ObAiModelEndpointInfo *endpoint = NULL;
      omt::ObAiServiceGuard guard;
      if (OB_FAIL(guard.get_ai_endpoint_by_ai_model_name(complete_model, endpoint, false))) {
      } else if (OB_ISNULL(endpoint)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, complete_model, info))) {
      } else if (OB_ISNULL(info)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObAIFuncModel model(allocator, *info, *endpoint);
        ObString result;
        if (OB_FAIL(model.call_completion(prompt_text, NULL, result))) {
        } else {
          ObObj result_obj;
          result_obj.set_varchar(result);
          result_obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
          result_obj.set_collation_level(CS_LEVEL_IMPLICIT);
          if (OB_FAIL(obj_to_sql_literal(result_obj, value_literal, allocator))) {
          } else {
            evaluated = true;
          }
        }
      }
    }
  } else if (!embed_model.empty() && !embed_input.empty()) {
    ObSqlString input_query;
    ObString input_text;
    if (OB_FAIL(input_query.append("SELECT ("))) {
    } else if (OB_FAIL(input_query.append(embed_input.ptr(), embed_input.length()))) {
    } else if (OB_FAIL(input_query.append(") FROM "))) {
    } else if (OB_FAIL(append_qualified_table(input_query, db_name, table_name))) {
    } else if (OB_FAIL(input_query.append(" WHERE "))) {
    } else if (OB_FAIL(input_query.append(pk_where.ptr()))) {
    }
    if (OB_SUCC(ret)) {
      SMART_VAR(ObMySQLProxy::MySQLResult, input_res) {
        ObObj input_obj;
        if (OB_FAIL(GCTX.sql_proxy_->read(input_res, input_query.ptr()))) {
        } else if (OB_ISNULL(input_res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_ITER_END == (ret = input_res.get_result()->next())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(ret)) {
        } else if (OB_FAIL(input_res.get_result()->get_obj(static_cast<int64_t>(0), input_obj))) {
        } else if (input_obj.is_null()) {
          ret = value_literal.append("NULL");
          evaluated = OB_SUCC(ret);
        } else {
          input_text = input_obj.get_string();
          if (input_text.empty()) {
            ret = value_literal.append("NULL");
            evaluated = OB_SUCC(ret);
          }
        }
      }
    }
    if (OB_SUCC(ret) && !evaluated) {
      ObAIFuncExprInfo *info = NULL;
      const ObAiModelEndpointInfo *endpoint = NULL;
      omt::ObAiServiceGuard guard;
      if (OB_FAIL(guard.get_ai_endpoint_by_ai_model_name(embed_model, endpoint, false))) {
      } else if (OB_ISNULL(endpoint)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, embed_model, info))) {
      } else if (OB_ISNULL(info)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObAIFuncModel model(allocator, *info, *endpoint);
        ObJsonObject *config = NULL;
        ObJsonInt *dim_json = NULL;
        ObString result;
        int64_t dimension = ai_column.get_ai_column_embed_dimension();
        if (dimension <= 0) {
          dimension = 4;
        }
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dimension, dim_json))) {
        } else if (OB_FAIL(config->add("dimensions", dim_json))) {
        } else if (OB_FAIL(model.call_dense_embedding(input_text, config, result))) {
        } else {
          ObObj result_obj;
          result_obj.set_varchar(result);
          result_obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
          result_obj.set_collation_level(CS_LEVEL_IMPLICIT);
          if (OB_FAIL(obj_to_sql_literal(result_obj, value_literal, allocator))) {
          } else {
            evaluated = true;
          }
        }
      }
    }
  } else {
    ObSqlString eval_query;
    if (OB_FAIL(eval_query.append("SELECT ("))) {
    } else if (OB_FAIL(eval_query.append(ai_column.get_ai_column_expr().ptr(),
                                         ai_column.get_ai_column_expr().length()))) {
    } else if (OB_FAIL(eval_query.append(") FROM "))) {
    } else if (OB_FAIL(append_qualified_table(eval_query, db_name, table_name))) {
    } else if (OB_FAIL(eval_query.append(" WHERE "))) {
    } else if (OB_FAIL(eval_query.append(pk_where.ptr()))) {
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, eval_res) {
        ObObj value_obj;
        if (OB_FAIL(GCTX.sql_proxy_->read(eval_res, eval_query.ptr()))) {
        } else if (OB_ISNULL(eval_res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_ITER_END == (ret = eval_res.get_result()->next())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(ret)) {
        } else if (OB_FAIL(eval_res.get_result()->get_obj(static_cast<int64_t>(0), value_obj))) {
        } else if (OB_FAIL(obj_to_sql_literal(value_obj, value_literal, allocator))) {
        } else {
          evaluated = true;
        }
      }
    }
  }
  return ret;
}

bool has_pending_status(const ObIArray<int64_t> &statuses)
{
  bool pending = false;
  for (int64_t i = 0; !pending && i < statuses.count(); ++i) {
    pending = (AI_COLUMN_STATUS_PENDING == statuses.at(i));
  }
  return pending;
}

} // namespace

ObAiColumnWorker::ObAiColumnWorker()
  : is_inited_(false),
    stop_(true),
    thread_()
{
}

ObAiColumnWorker::~ObAiColumnWorker()
{
  destroy();
}

int ObAiColumnWorker::init()
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObAiColumnWorker::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (!stop_) {
    ret = OB_INIT_TWICE;
  } else {
    stop_ = false;
    thread_ = std::thread([this]() { run_(); });
  }
  return ret;
}

void ObAiColumnWorker::stop()
{
  stop_ = true;
}

void ObAiColumnWorker::wait()
{
  if (thread_.joinable()) {
    thread_.join();
  }
}

void ObAiColumnWorker::destroy()
{
  stop();
  wait();
  is_inited_ = false;
}

void ObAiColumnWorker::run_()
{
  int ret = OB_SUCCESS;
  while (!stop_) {
    if (OB_FAIL(process_once_())) {
      if (OB_SUCCESS != ret) {
        LOG_WARN("process ai column once failed", KR(ret));
      }
    }
    for (int64_t i = 0; !stop_ && i < 10; ++i) {
      usleep(100 * 1000);
    }
  }
}

int ObAiColumnWorker::process_once_()
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  ObSEArray<uint64_t, 32> table_ids;
  if (OB_ISNULL(GCTX.schema_service_) || OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_ids_in_runtime(table_ids))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < table_ids.count() && !stop_; ++i) {
      const ObTableSchema *table_schema = NULL;
      bool has_ai_column = false;
      if (OB_FAIL(guard.get_table_schema(table_ids.at(i), table_schema))) {
      } else if (OB_ISNULL(table_schema) || !table_schema->is_user_table()) {
        // skip
      } else {
        for (ObTableSchema::const_column_iterator iter = table_schema->column_begin();
             !has_ai_column && iter != table_schema->column_end(); ++iter) {
          has_ai_column = OB_NOT_NULL(*iter) && (*iter)->is_ai_column();
        }
        if (has_ai_column && OB_FAIL(process_table_(*table_schema))) {
          LOG_WARN("process ai column table failed", KR(ret),
                   "table_id", table_schema->get_table_id());
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::process_table_(const ObTableSchema &table_schema)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  const ObDatabaseSchema *db_schema = NULL;
  ObSEArray<const ObColumnSchemaV2 *, 4> ai_columns;
  ObSEArray<const ObColumnSchemaV2 *, 4> pk_columns;
  if (OB_ISNULL(GCTX.schema_service_) || OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_database_schema(table_schema.get_database_id(), db_schema))) {
  } else if (OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (ObTableSchema::const_column_iterator iter = table_schema.column_begin();
         OB_SUCC(ret) && iter != table_schema.column_end(); ++iter) {
      const ObColumnSchemaV2 *column = *iter;
      if (OB_ISNULL(column)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (column->is_ai_column()
                 && (OB_INVALID_ID == column->get_ai_column_status_column_id()
                     || OB_INVALID_ID == column->get_ai_column_gen_column_id())) {
        // skip columns created by an incompatible older definition
      } else if (column->is_ai_column() && OB_FAIL(ai_columns.push_back(column))) {
      } else if (column->is_rowkey_column() && OB_FAIL(pk_columns.push_back(column))) {
      }
    }
  }

  if (OB_SUCC(ret) && ai_columns.empty()) {
    // nothing to do
  } else if (OB_SUCC(ret) && pk_columns.empty()) {
    ret = OB_ERR_UNEXPECTED;
  }

  const ObString &db_name = db_schema->get_database_name_str();
  const ObString &table_name = table_schema.get_table_name_str();
  for (int64_t round = 0; OB_SUCC(ret) && !stop_
                         && round < AI_COLUMN_WORKER_MAX_ROWS_PER_TABLE; ++round) {
    ObArenaAllocator allocator("AiColWorker");
    ObSqlString query;
    ObSEArray<ObObj, 8> pk_values;
    ObSEArray<int64_t, 8> statuses;
    int64_t gen_value = 0;

    if (OB_FAIL(query.append("SELECT "))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < pk_columns.count(); ++i) {
      if (i > 0 && OB_FAIL(query.append(", "))) {
      } else if (OB_FAIL(append_identifier(query, pk_columns.at(i)->get_column_name_str()))) {
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
      const uint64_t status_column_id = ai_columns.at(i)->get_ai_column_status_column_id();
      const ObColumnSchemaV2 *status_column = table_schema.get_column_schema(status_column_id);
      if (OB_ISNULL(status_column)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(query.append(", "))) {
      } else if (OB_FAIL(append_identifier(query, status_column->get_column_name_str()))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(query.append(" FROM "))) {
    } else if (OB_FAIL(append_qualified_table(query, db_name, table_name))) {
    } else if (OB_FAIL(query.append(" WHERE "))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
      const ObColumnSchemaV2 *status_column =
          table_schema.get_column_schema(ai_columns.at(i)->get_ai_column_status_column_id());
      if (OB_ISNULL(status_column)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (i > 0 && OB_FAIL(query.append(" OR "))) {
      } else if (OB_FAIL(append_identifier(query, status_column->get_column_name_str()))) {
      } else if (OB_FAIL(query.append_fmt(" = %ld", AI_COLUMN_STATUS_PENDING))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(query.append(" LIMIT 1"))) {
    }

    bool found_row = false;
    if (OB_SUCC(ret)) {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        if (OB_FAIL(GCTX.sql_proxy_->read(res, query.ptr()))) {
        } else if (OB_ISNULL(res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          sqlclient::ObMySQLResult *result = res.get_result();
          if (OB_ITER_END == (ret = result->next())) {
            ret = OB_SUCCESS;
          } else if (OB_FAIL(ret)) {
          } else {
            found_row = true;
            for (int64_t i = 0; OB_SUCC(ret) && i < pk_columns.count(); ++i) {
              ObObj obj;
              if (OB_FAIL(result->get_obj(i, obj))) {
              } else if (OB_FAIL(pk_values.push_back(obj))) {
              }
            }
            for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
              int64_t status = AI_COLUMN_STATUS_PENDING;
              if (OB_FAIL(result->get_int(static_cast<int64_t>(pk_columns.count()) + i, status))) {
              } else if (OB_FAIL(statuses.push_back(status))) {
              }
            }
          }
        }
      }
    }

    if (OB_SUCC(ret) && !found_row) {
      break;
    }
    if (OB_SUCC(ret) && !has_pending_status(statuses)) {
      break;
    }

    ObSqlString pk_where;
    for (int64_t i = 0; OB_SUCC(ret) && i < pk_columns.count(); ++i) {
      if (i > 0 && OB_FAIL(pk_where.append(" AND "))) {
      } else if (OB_FAIL(append_identifier(pk_where, pk_columns.at(i)->get_column_name_str()))) {
      } else if (OB_FAIL(pk_where.append(" <=> "))) {
      } else if (OB_FAIL(obj_to_sql_literal(pk_values.at(i), pk_where, allocator))) {
      }
    }

    const ObColumnSchemaV2 *first_ai_column = ai_columns.at(0);
    const uint64_t gen_column_id = first_ai_column->get_ai_column_gen_column_id();
    const ObColumnSchemaV2 *gen_column = table_schema.get_column_schema(gen_column_id);
    bool row_gone = false;
    if (OB_SUCC(ret) && OB_NOT_NULL(gen_column)) {
      ObSqlString gen_query;
      if (OB_FAIL(gen_query.append("SELECT "))) {
      } else if (OB_FAIL(append_identifier(gen_query, gen_column->get_column_name_str()))) {
      } else if (OB_FAIL(gen_query.append(" FROM "))) {
      } else if (OB_FAIL(append_qualified_table(gen_query, db_name, table_name))) {
      } else if (OB_FAIL(gen_query.append(" WHERE "))) {
      } else if (OB_FAIL(gen_query.append(pk_where.ptr()))) {
      }
      if (OB_SUCC(ret)) {
        SMART_VAR(ObMySQLProxy::MySQLResult, gen_res) {
          if (OB_FAIL(GCTX.sql_proxy_->read(gen_res, gen_query.ptr()))) {
          } else if (OB_ISNULL(gen_res.get_result())) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_ITER_END == (ret = gen_res.get_result()->next())) {
            ret = OB_SUCCESS;
            row_gone = true;
          } else if (OB_FAIL(ret)) {
          } else if (OB_FAIL(gen_res.get_result()->get_int(static_cast<int64_t>(0), gen_value))) {
          }
        }
      }
    } else if (OB_SUCC(ret)) {
      ret = OB_ERR_UNEXPECTED;
    }
    if (OB_SUCC(ret) && row_gone) {
      break;
    }

    ObSqlString update;
    bool has_set_clause = false;
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
      if (AI_COLUMN_STATUS_PENDING != statuses.at(i)) {
        continue;
      }
      const ObColumnSchemaV2 *ai_column = ai_columns.at(i);
      const ObColumnSchemaV2 *status_column =
          table_schema.get_column_schema(ai_column->get_ai_column_status_column_id());
      bool evaluated = false;
      ObSqlString value_literal;
      if (OB_ISNULL(status_column)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        const int64_t max_attempts = 1 + ai_column->get_ai_column_max_retries();
        for (int64_t attempt = 0;
             OB_SUCC(ret) && !evaluated && attempt < max_attempts; ++attempt) {
          if (attempt > 0) {
            usleep(50 * 1000);
          }
          value_literal.reuse();
          int eval_ret = evaluate_ai_column_value(table_schema,
                                                  *ai_column,
                                                  db_name,
                                                  table_name,
                                                  pk_where,
                                                  allocator,
                                                  evaluated,
                                                  value_literal);
          if (OB_SUCCESS != eval_ret) {
            LOG_WARN("evaluate ai column failed", K(eval_ret),
                     "table_id", table_schema.get_table_id(),
                     "column_id", ai_column->get_column_id(), K(attempt));
          }
        }
        if (OB_SUCC(ret) && !evaluated) {
          LOG_WARN("ai column evaluation exhausted retries",
                   "table_id", table_schema.get_table_id(),
                   "column_id", ai_column->get_column_id(),
                   "max_attempts", max_attempts);
        }
      }

      if (OB_FAIL(ret)) {
      } else if (!has_set_clause) {
        if (OB_FAIL(update.append("UPDATE "))) {
        } else if (OB_FAIL(append_qualified_table(update, db_name, table_name))) {
        } else if (OB_FAIL(update.append(" SET "))) {
        }
        has_set_clause = true;
      } else if (OB_FAIL(update.append(", "))) {
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(append_identifier(update, ai_column->get_column_name_str()))) {
        } else if (OB_FAIL(update.append(" = "))) {
        } else if (evaluated) {
          if (OB_FAIL(update.append(value_literal.ptr()))) {
          }
        } else if (OB_FAIL(update.append("NULL"))) {
        }
        if (OB_SUCC(ret) && OB_FAIL(update.append(", "))) {
        } else if (OB_SUCC(ret)
                   && OB_FAIL(append_identifier(update, status_column->get_column_name_str()))) {
        } else if (OB_SUCC(ret)
                   && OB_FAIL(update.append_fmt(" = %ld",
                          evaluated ? AI_COLUMN_STATUS_READY : AI_COLUMN_STATUS_FAILED))) {
        }
      }
    }

    if (OB_FAIL(ret)) {
    } else if (!has_set_clause) {
      break;
    } else if (OB_FAIL(update.append(" WHERE "))) {
    } else if (OB_FAIL(update.append(pk_where.ptr()))) {
    } else if (OB_NOT_NULL(gen_column)
               && OB_FAIL(update.append_fmt(" AND "))) {
    }
    if (OB_SUCC(ret) && OB_NOT_NULL(gen_column)) {
      if (OB_FAIL(append_identifier(update, gen_column->get_column_name_str()))) {
      } else if (OB_FAIL(update.append_fmt(" = %ld", gen_value))) {
      }
    }

    if (OB_SUCC(ret)) {
      int64_t affected_rows = 0;
      if (OB_FAIL(GCTX.sql_proxy_->write(update.ptr(), affected_rows))) {
        LOG_WARN("publish ai column failed", KR(ret), K(update));
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
