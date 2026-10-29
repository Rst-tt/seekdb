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

#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/allocator/page_arena.h"
#include "lib/string/ob_sql_string.h"
#include "lib/utility/utility.h"
#include "share/ob_server_struct.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "observer/omt/ob_ai_service.h"
#include "observer/ob_inner_sql_connection.h"
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
constexpr int64_t AI_COLUMN_RETRYING_STATUS = 3;
constexpr int64_t AI_COLUMN_WORKER_MAX_ROWS_PER_TABLE = 32;
constexpr int64_t AI_COLUMN_WORKER_IDLE_SLEEP_MS = 10;

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

int append_sql_string(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
      if ('\'' == value[i]) {
        ret = sql.append("''", 2);
      } else {
        ret = sql.append(value.ptr() + i, 1);
      }
    }
    if (OB_SUCC(ret)) {
      ret = sql.append("'");
    }
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
                             const ObString &pk_where,
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
    } else if (OB_FAIL(prompt_query.append(pk_where.ptr(), pk_where.length()))) {
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
        } else if (OB_FAIL(ob_write_string(
                       allocator, prompt_obj.get_string(), prompt_text))) {
        } else {
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
        ObSqlString system_prompt_buf;
        ObString system_prompt;
        ObString result;
        if (OB_FAIL(system_prompt_buf.append(
                "Generate the value for this AI column expression: "))) {
        } else if (OB_FAIL(system_prompt_buf.append(
                       ai_column.get_ai_column_expr().ptr(),
                       ai_column.get_ai_column_expr().length()))) {
        } else if (ai_column.is_json()
                   && OB_FAIL(system_prompt_buf.append(
                          " Return exactly one valid JSON value."))) {
        } else if (ob_is_integer_type(ai_column.get_data_type())
                   && OB_FAIL(system_prompt_buf.append(
                          " Return a base-10 integer."))) {
        } else if (FALSE_IT(system_prompt = system_prompt_buf.string())) {
        } else if (OB_FAIL(model.call_completion_with_system(
                       system_prompt, prompt_text, NULL, result))) {
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
    } else if (OB_FAIL(input_query.append(pk_where.ptr(), pk_where.length()))) {
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
        } else if (OB_FAIL(ob_write_string(
                       allocator, input_obj.get_string(), input_text))) {
        } else {
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
    } else if (OB_FAIL(eval_query.append(pk_where.ptr(), pk_where.length()))) {
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

struct AiColumnTask
{
  AiColumnTask()
    : task_id_(0), column_id_(0), state_(AI_COLUMN_STATUS_PENDING), retry_count_(0)
  {
  }
  int64_t task_id_;
  int64_t column_id_;
  int64_t state_;
  int64_t retry_count_;
};

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
    ob_usleep(AI_COLUMN_WORKER_IDLE_SLEEP_MS * 1000);
  }
}

int ObAiColumnWorker::process_once_()
{
  int ret = OB_SUCCESS;
  int64_t task_id = 0;
  int64_t table_id = 0;
  int64_t column_id = 0;
  int64_t request_id = 0;
  int64_t generation = 0;
  ObString rowkey_predicate;
  ObArenaAllocator allocator("AiTaskWorker");
  ObSchemaGetterGuard guard;
  const ObTableSchema *task_table_schema = NULL;
  if (OB_ISNULL(GCTX.sql_proxy_) || OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(OB_ALL_AI_COLUMN_TASK_TID,
                                            task_table_schema))) {
  } else if (OB_ISNULL(task_table_schema)) {
    // The AI task table appears only after bootstrap has finished.
  } else {
    ObSqlString sql;
    bool found = false;
    if (OB_FAIL(sql.append(
            "SELECT task_id, table_id, column_id, request_id, rowkey_predicate, generation "
            "FROM oceanbase.__all_ai_column_task "
            "WHERE state IN (0,3) AND eligible_ts <= now(6) "
            "ORDER BY eligible_ts, task_id LIMIT 1"))) {
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        } else if (OB_ISNULL(res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          sqlclient::ObMySQLResult *result = res.get_result();
          int tmp_ret = result->next();
          if (OB_SUCCESS == tmp_ret) {
            found = true;
            if (OB_FAIL(result->get_int(static_cast<int64_t>(0), task_id))) {
            } else if (OB_FAIL(result->get_int(static_cast<int64_t>(1), table_id))) {
            } else if (OB_FAIL(result->get_int(static_cast<int64_t>(2), column_id))) {
            } else if (OB_FAIL(result->get_int(static_cast<int64_t>(3), request_id))) {
            } else {
              ObString raw_predicate;
              if (OB_FAIL(result->get_varchar(
                             static_cast<int64_t>(4), raw_predicate))) {
              } else if (OB_FAIL(ob_write_string(
                             allocator, raw_predicate, rowkey_predicate))) {
              }
            }
            if (OB_FAIL(ret)) {
            } else if (OB_FAIL(result->get_int(static_cast<int64_t>(5), generation))) {
            }
          } else if (OB_ITER_END == tmp_ret) {
            ret = OB_SUCCESS;
          } else {
            ret = tmp_ret;
          }
        }
      }
    }
    UNUSED(task_id);
    UNUSED(column_id);
    if (OB_FAIL(ret) || !found || stop_) {
      // no task
    } else {
      ret = process_ai_task_group_(table_id, request_id, generation,
                                   rowkey_predicate, allocator);
    }
  }
  return ret;
}

int ObAiColumnWorker::process_ai_task_group_(const uint64_t table_id,
                                             const int64_t request_id,
                                             const int64_t generation,
                                             const ObString &rowkey_predicate,
                                             ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table_schema = NULL;
  const ObDatabaseSchema *db_schema = NULL;
  ObSEArray<AiColumnTask, 4> tasks;
  if (OB_ISNULL(GCTX.schema_service_) || OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else {
    ObSqlString sql;
    if (OB_FAIL(sql.append_fmt(
            "SELECT task_id, column_id, state, retry_count "
            "FROM oceanbase.__all_ai_column_task "
            "WHERE table_id = %lu AND request_id = %ld AND generation = %ld "
            "AND rowkey_predicate = ",
            table_id, request_id, generation))) {
    } else if (OB_FAIL(append_sql_string(sql, rowkey_predicate))) {
    } else if (OB_FAIL(sql.append(" ORDER BY task_id"))) {
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        } else if (OB_ISNULL(res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          sqlclient::ObMySQLResult *result = res.get_result();
          while (OB_SUCC(ret)) {
            int next_ret = result->next();
            if (OB_ITER_END == next_ret) {
              break;
            } else if (OB_FAIL(next_ret)) {
            } else {
              AiColumnTask task;
              if (OB_FAIL(result->get_int(static_cast<int64_t>(0), task.task_id_))) {
              } else if (OB_FAIL(result->get_int(static_cast<int64_t>(1), task.column_id_))) {
              } else if (OB_FAIL(result->get_int(static_cast<int64_t>(2), task.state_))) {
              } else if (OB_FAIL(result->get_int(static_cast<int64_t>(3), task.retry_count_))) {
              } else if (OB_FAIL(tasks.push_back(task))) {
              }
            }
          }
        }
      }
    }
  }

  ObSqlString delete_tasks;
  if (OB_SUCC(ret) && tasks.empty()) {
    // Another worker or the owning transaction already removed this group.
  } else if (OB_SUCC(ret)
             && OB_FAIL(delete_tasks.append_fmt(
                    "DELETE FROM oceanbase.__all_ai_column_task "
                    "WHERE table_id = %lu AND request_id = %ld AND generation = %ld "
                    "AND rowkey_predicate = ",
                    table_id, request_id, generation))) {
  } else if (OB_SUCC(ret) && OB_FAIL(append_sql_string(delete_tasks, rowkey_predicate))) {
  } else if (OB_SUCC(ret)
             && OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_SUCC(ret) && OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
  } else if (OB_SUCC(ret) && OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
  } else if (OB_SUCC(ret)
             && OB_FAIL(guard.get_database_schema(table_schema->get_database_id(), db_schema))) {
  } else if (OB_SUCC(ret) && OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
  }

  const ObString &db_name = OB_NOT_NULL(db_schema)
      ? db_schema->get_database_name_str() : ObString();
  const ObString &table_name = OB_NOT_NULL(table_schema)
      ? table_schema->get_table_name_str() : ObString();
  if (OB_SUCC(ret) && !tasks.empty()) {
    ObSqlString set_clause;
    bool all_succeeded = true;
    bool can_retry = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
      const AiColumnTask &task = tasks.at(i);
      const ObColumnSchemaV2 *ai_column =
          table_schema->get_column_schema(static_cast<uint64_t>(task.column_id_));
      const ObColumnSchemaV2 *status_column = OB_NOT_NULL(ai_column)
          ? table_schema->get_column_schema(ai_column->get_ai_column_status_column_id())
          : NULL;
      if (OB_ISNULL(ai_column) || !ai_column->is_ai_column()
          || OB_ISNULL(status_column)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        if (task.retry_count_ >= ai_column->get_ai_column_max_retries()) {
          can_retry = false;
        }
        bool evaluated = false;
        ObSqlString value_literal;
        int eval_ret = evaluate_ai_column_value(*table_schema,
                                                *ai_column,
                                                db_name,
                                                table_name,
                                                rowkey_predicate,
                                                allocator,
                                                evaluated,
                                                value_literal);
        if (OB_SUCCESS != eval_ret || !evaluated) {
          all_succeeded = false;
          LOG_WARN("evaluate ai task failed", K(eval_ret),
                   K(table_id), K(task.column_id_), K(request_id),
                   K(task.retry_count_));
        }
        if (OB_SUCC(ret) && all_succeeded) {
          if (i > 0 && OB_FAIL(set_clause.append(", "))) {
          } else if (OB_FAIL(append_identifier(
                         set_clause, ai_column->get_column_name_str()))) {
          } else if (OB_FAIL(set_clause.append(" = "))) {
          } else if (OB_FAIL(set_clause.append(value_literal.ptr()))) {
          } else if (ai_column->is_collection()) {
            // Updating an AI vector together with its hidden status causes the
            // vector index maintenance path to treat the same row as both an
            // update and an insert. The vector value itself is the readiness
            // signal for this column.
          } else if (OB_FAIL(set_clause.append(", "))) {
          } else if (OB_FAIL(append_identifier(
                         set_clause, status_column->get_column_name_str()))) {
          } else if (OB_FAIL(set_clause.append_fmt(
                         " = %ld", AI_COLUMN_STATUS_READY))) {
          }
        }
      }
    }

    if (OB_FAIL(ret)) {
    } else if (!all_succeeded && can_retry) {
      ObSqlString update_retry;
      if (OB_FAIL(update_retry.append_fmt(
              "UPDATE oceanbase.__all_ai_column_task "
              "SET state = %ld, retry_count = retry_count + 1, eligible_ts = now(6) "
              "WHERE table_id = %lu AND request_id = %ld AND generation = %ld "
              "AND rowkey_predicate = ",
              AI_COLUMN_RETRYING_STATUS, table_id, request_id, generation))) {
      } else if (OB_FAIL(append_sql_string(update_retry, rowkey_predicate))) {
      } else {
        int64_t affected_rows = 0;
        if (OB_FAIL(GCTX.sql_proxy_->write(update_retry.ptr(), affected_rows))) {
          LOG_WARN("schedule ai column retry failed", KR(ret), K(update_retry));
        }
      }
    } else {
      ObSqlString publish_sql;
      bool need_publish = false;
      if (all_succeeded) {
        if (OB_FAIL(publish_sql.append("UPDATE "))) {
        } else if (OB_FAIL(append_qualified_table(
                       publish_sql, db_name, table_name))) {
        } else if (OB_FAIL(publish_sql.append(" SET "))) {
        } else if (OB_FAIL(publish_sql.append(set_clause.ptr()))) {
        } else if (OB_FAIL(publish_sql.append(" WHERE "))) {
        } else if (OB_FAIL(publish_sql.append(
                       rowkey_predicate.ptr(), rowkey_predicate.length()))) {
        } else {
          need_publish = true;
        }
      } else if (!can_retry) {
        if (OB_FAIL(publish_sql.append("UPDATE "))) {
        } else if (OB_FAIL(append_qualified_table(
                       publish_sql, db_name, table_name))) {
        } else if (OB_FAIL(publish_sql.append(" SET "))) {
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
          const ObColumnSchemaV2 *ai_column =
              table_schema->get_column_schema(static_cast<uint64_t>(tasks.at(i).column_id_));
          const ObColumnSchemaV2 *status_column = OB_NOT_NULL(ai_column)
              ? table_schema->get_column_schema(ai_column->get_ai_column_status_column_id())
              : NULL;
          if (OB_ISNULL(status_column)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (i > 0 && OB_FAIL(publish_sql.append(", "))) {
          } else if (OB_FAIL(append_identifier(
                         publish_sql, status_column->get_column_name_str()))) {
          } else if (OB_FAIL(publish_sql.append_fmt(
                         " = %ld", AI_COLUMN_STATUS_FAILED))) {
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(publish_sql.append(" WHERE "))) {
        } else if (OB_FAIL(publish_sql.append(
                       rowkey_predicate.ptr(), rowkey_predicate.length()))) {
        } else {
          need_publish = true;
        }
      }
      int64_t deleted_rows = 0;
      if (OB_SUCC(ret)
          && OB_FAIL(GCTX.sql_proxy_->write(delete_tasks.ptr(), deleted_rows))) {
      } else if (OB_SUCC(ret) && deleted_rows == tasks.count()) {
        if (need_publish) {
          ObISQLConnectionGuard conn_guard;
          ObInnerSQLConnection *inner_conn = NULL;
          int64_t affected_rows = 0;
          if (OB_FAIL(ObInnerSQLConnection::create_connection_with_owned_session(
                         false, 0, conn_guard))) {
          } else if (OB_ISNULL(inner_conn =
                         dynamic_cast<ObInnerSQLConnection *>(conn_guard.get_ptr()))) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(inner_conn->execute_write(
                         publish_sql.string(), affected_rows, true))) {
            LOG_WARN("publish ai column result failed", KR(ret), K(publish_sql));
          }
        }
      }
      LOG_TRACE("finish ai worker publish", KR(ret), K(deleted_rows),
                K(all_succeeded), K(tasks.count()));
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
      } else if (OB_FAIL(gen_query.append(pk_where.ptr(), pk_where.length()))) {
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
            ob_usleep(50 * 1000);
          }
          value_literal.reuse();
          int eval_ret = evaluate_ai_column_value(table_schema,
                                                  *ai_column,
                                                  db_name,
                                                  table_name,
                                                  pk_where.string(),
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
    } else if (OB_FAIL(update.append(pk_where.ptr(), pk_where.length()))) {
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
