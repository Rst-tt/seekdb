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

#define USING_LOG_PREFIX SQL_OPT
#include "sql/optimizer/ob_explain_log_plan.h"
#include "sql/optimizer/ob_log_values.h"
#include "sql/code_generator/ob_code_generator.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_file_table_def.h"

using namespace oceanbase;
using namespace sql;
using namespace oceanbase::common;
using namespace oceanbase::json;
using namespace oceanbase::sql::log_op_def;

/**
 *  This function does two things:
 *
 *  1. generate a logical plan for the 'real' query that's being 'explained'
 *  2. generate the plan text from the logical plan and put the text in the buffer
 *     and remember the logical plan as well.
 */
int ObExplainLogPlan::generate_normal_raw_plan()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(get_stmt()) || OB_UNLIKELY(!get_stmt()->is_explain_stmt())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (static_cast<const ObExplainStmt*>(get_stmt())->is_describe()) {
    ret = generate_describe_plan();
  } else {
    ObLogPlan *child_plan = NULL;
    const ObDMLStmt *child_stmt = NULL;
    ObLogicalOperator *top = NULL;
    ObLogValues *values_op = NULL;
    int64_t batch_size = 0;
    const ObExplainStmt *explain_stmt = static_cast<const ObExplainStmt*>(get_stmt());
    if (OB_ISNULL(child_stmt = explain_stmt->get_explain_query_stmt())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_ISNULL(child_plan = optimizer_context_.get_log_plan_factory().
                         create(optimizer_context_, *child_stmt))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("failed to create log plan for explain stmt");
    } else if (OB_FAIL(child_plan->generate_plan())) {
    } else if (OB_FAIL(remove_duplicate_constraints())) {
    } else if (OB_FAIL(check_explain_generate_plan_with_outline(child_plan))) {
    } else if (OB_FAIL(ObCodeGenerator::detect_batch_size(*child_plan, batch_size))) {
    } else if (OB_FAIL(allocate_values_as_top(top))) {
    } else if (OB_ISNULL(top) || OB_UNLIKELY(LOG_VALUES != top->get_type())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      set_plan_root(top);
      top->mark_is_plan_root();
      child_plan->get_optimizer_context().set_batch_size(batch_size);
      values_op = static_cast<ObLogValues*>(top);
      values_op->set_explain_plan(child_plan);
      ObSqlPlan sql_plan(get_allocator());
      sql_plan.set_session_info(get_optimizer_context().get_session_info());
      ObExplainLogPlan *explain_plan = static_cast<ObExplainLogPlan*>(child_plan);
      ObSEArray<common::ObString, 64> plan_strs;
      const ObString& into_table = explain_stmt->get_into_table();
      const ObString& statement_id = explain_stmt->get_statement_id();
      if (OB_FAIL(sql_plan.store_sql_plan_for_explain(get_optimizer_context().get_exec_ctx(),
                                                      child_plan,
                                                      explain_stmt->get_explain_type(),
                                                      0 == into_table.length() ? "PLAN_TABLE" : into_table,
                                                      0 == statement_id.length() ? "" : statement_id,
                                                      explain_stmt->get_display_opt(),
                                                      plan_strs))) {
      } else {
        //For explain stmt, we can do pack at the stage of expr alloc,
        //But we need to use the FALSE flag to tell the driver 
        //to use normal encoding instead of memcopy
        optimizer_context_.set_packed(false);
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < plan_strs.count(); ++i) {
        ObObj obj;
        obj.set_varchar(plan_strs.at(i));
        ObNewRow row;
        row.cells_ = &obj;
        row.count_ = 1;
        if (OB_FAIL(values_op->add_row(row))) {
        }
      }
      if (OB_SUCC(ret)) {
        // set values_op operator id && set max operator id for LogPlan
        values_op->set_op_id(0);
        set_max_op_id(1);
      }
    }
    get_optimizer_context().get_all_exprs().reuse();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(allocate_output_expr_for_values_op(*values_op))) {
    } else {
      get_optimizer_context().set_plan_type(ObPhyPlanType::OB_PHY_PLAN_LOCAL,
                                            ObPhyPlanType::OB_PHY_PLAN_LOCAL,
                                            false);
    }
  }
  return ret;
}

const char *ObExplainLogPlan::get_describe_type_name(const ObObjTypeClass tc, const ObObjType type)
{
  const char *name = "VARCHAR";
  switch (tc) {
    case ObIntTC:
    case ObUIntTC:
      name = "BIGINT";
      break;
    case ObFloatTC:
    case ObDoubleTC:
    case ObNumberTC:
      name = "DOUBLE";
      break;
    case ObDateTC:
      name = "DATE";
      break;
    case ObDateTimeTC:
      name = "DATETIME";
      break;
    case ObNullTC:
      name = "NULL";
      break;
    case ObStringTC:
    case ObTextTC:
    case ObJsonTC:
    case ObIntervalTC:
    case ObRowIDTC:
    case ObLobTC:
    case ObExtendTC:
    case ObUnknownTC:
    case ObMaxTC:
      name = (ObJsonTC == tc) ? "JSON" : "VARCHAR";
      break;
    case ObOTimestampTC:
      name = "DATETIME";
      break;
    case ObBitTC:
    case ObEnumSetTC:
      name = "VARCHAR";
      break;
    default:
      name = "VARCHAR";
      break;
  }
  UNUSED(type);
  return name;
}

int ObExplainLogPlan::fill_describe_rows(const ObDMLStmt &child_stmt, ObLogValues &values_op)
{
  int ret = OB_SUCCESS;
  const TableItem *file_table = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && i < child_stmt.get_table_size(); ++i) {
    const TableItem *table_item = child_stmt.get_table_item(i);
    if (OB_ISNULL(table_item)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (table_item->is_file_table() && NULL != table_item->file_table_def_) {
      file_table = table_item;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (NULL != file_table) {
    const ObFileTableDef *def = file_table->file_table_def_;
    for (int64_t i = 0; OB_SUCC(ret) && i < def->column_cnt_; ++i) {
      const ObFileColumnDef &col = def->columns_[i];
      ObObj cells[OB_DESCRIBE_COLUMN_COUNT];
      cells[0].set_varchar(col.name_);
      cells[1].set_varchar(ObString::make_string(def->get_type_name(i)));
      cells[2].set_varchar(ObString::make_string(col.nullable_ ? "YES" : "NO"));
      cells[3].set_null();
      cells[4].set_null();
      cells[5].set_null();
      for (int64_t k = 0; k < 3; ++k) {
        cells[k].set_collation_type(ObCharset::get_system_collation());
        cells[k].set_collation_level(CS_LEVEL_IMPLICIT);
      }
      ObNewRow row;
      row.cells_ = cells;
      row.count_ = OB_DESCRIBE_COLUMN_COUNT;
      if (OB_FAIL(values_op.add_row(row))) {
      }
    }
  } else if (OB_UNLIKELY(!child_stmt.is_select_stmt())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const ObSelectStmt &select_stmt = static_cast<const ObSelectStmt&>(child_stmt);
    for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt.get_select_item_size(); ++i) {
      const SelectItem &item = select_stmt.get_select_item(i);
      const ObRawExpr *expr = item.expr_;
      ObString col_name = item.alias_name_;
      if (col_name.empty() && NULL != expr && expr->is_column_ref_expr()) {
        col_name = static_cast<const ObColumnRefRawExpr*>(expr)->get_column_name();
      }
      if (col_name.empty()) {
        col_name = ObString::make_string("column");
      }
      bool nullable = true;
      const char *type_name = "VARCHAR";
      if (NULL != expr) {
        nullable = !expr->get_result_type().has_result_flag(NOT_NULL_FLAG);
        type_name = get_describe_type_name(expr->get_type_class(), expr->get_data_type());
      }
      ObObj cells[OB_DESCRIBE_COLUMN_COUNT];
      cells[0].set_varchar(col_name);
      cells[1].set_varchar(ObString::make_string(type_name));
      cells[2].set_varchar(ObString::make_string(nullable ? "YES" : "NO"));
      cells[3].set_null();
      cells[4].set_null();
      cells[5].set_null();
      ObNewRow row;
      row.cells_ = cells;
      row.count_ = OB_DESCRIBE_COLUMN_COUNT;
      if (OB_FAIL(values_op.add_row(row))) {
      }
    }
  }
  return ret;
}

int ObExplainLogPlan::allocate_describe_output_expr(ObLogicalOperator &values_op)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(optimizer_context_.get_session_info())) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < OB_DESCRIBE_COLUMN_COUNT; ++i) {
    ObConstRawExpr *output = NULL;
    if (OB_FAIL(optimizer_context_.get_expr_factory().create_raw_expr(T_VARCHAR, output))) {
    } else {
      ObObj v;
      v.set_varchar(" ");
      v.set_collation_type(ObCharset::get_system_collation());
      output->set_value(v);
      if (OB_FAIL(output->formalize(optimizer_context_.get_session_info()))) {
      } else if (OB_FAIL(values_op.get_output_exprs().push_back(output))) {
      } else if (OB_FAIL(get_optimizer_context().get_all_exprs().append(output))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    values_op.set_branch_id(0);
    values_op.set_id(0);
    values_op.set_op_id(0);
  }
  return ret;
}

int ObExplainLogPlan::generate_describe_plan()
{
  int ret = OB_SUCCESS;
  const ObExplainStmt *explain_stmt = static_cast<const ObExplainStmt*>(get_stmt());
  const ObDMLStmt *child_stmt = NULL;
  ObLogicalOperator *top = NULL;
  ObLogValues *values_op = NULL;
  if (OB_ISNULL(explain_stmt) || OB_ISNULL(child_stmt = explain_stmt->get_explain_query_stmt())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(allocate_values_as_top(top))) {
  } else if (OB_ISNULL(top) || OB_UNLIKELY(LOG_VALUES != top->get_type())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(values_op = static_cast<ObLogValues*>(top))) {
  } else if (OB_FAIL(fill_describe_rows(*child_stmt, *values_op))) {
  } else {
    set_plan_root(top);
    top->mark_is_plan_root();
    values_op->set_op_id(0);
    set_max_op_id(1);
    get_optimizer_context().get_all_exprs().reuse();
    if (OB_FAIL(allocate_describe_output_expr(*values_op))) {
    } else {
      get_optimizer_context().set_plan_type(ObPhyPlanType::OB_PHY_PLAN_LOCAL,
                                            ObPhyPlanType::OB_PHY_PLAN_LOCAL,
                                            false);
    }
  }
  return ret;
}

int ObExplainLogPlan::check_explain_generate_plan_with_outline(ObLogPlan *real_plan)
{
  int ret = OB_SUCCESS;
  ObExecContext *exec_ctx = NULL;
  ObSqlCtx *sql_ctx = NULL;
  ObSQLSessionInfo *session_info = NULL;
  const ObExplainStmt *explain_stmt = static_cast<const ObExplainStmt*>(get_stmt());
  ObQueryCtx *query_ctx = get_optimizer_context().get_query_ctx();
  if (OB_ISNULL(real_plan) || OB_ISNULL(explain_stmt)
      || OB_ISNULL(exec_ctx = get_optimizer_context().get_exec_ctx())
      || OB_ISNULL(session_info = get_optimizer_context().get_session_info())
      || OB_ISNULL(sql_ctx = exec_ctx->get_sql_ctx()) || OB_ISNULL(query_ctx)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (session_info->is_inner() || sql_ctx->is_prepare_protocol_) {
    /* do not check explain for inner sql (include query in PL) */
  } else if (OB_UNLIKELY(get_optimizer_context().get_query_ctx() != NULL
                         && get_optimizer_context().get_query_ctx()->get_injected_random_status())) { 
    /* do nothing */
  } else if (sql_ctx->multi_stmt_item_.is_part_of_multi_stmt()
             && sql_ctx->multi_stmt_item_.get_seq_num() > 0) {
    /* generate plan call by ObMPQuery::process_with_tmp_context use tmp context, do not check */
  } else if (EXPLAIN_UNINITIALIZED !=  explain_stmt->get_explain_type()
             && EXPLAIN_BASIC !=  explain_stmt->get_explain_type()
             && EXPLAIN_OUTLINE !=  explain_stmt->get_explain_type()) {
    /* generate plan again for explain/explain basic/explain outline,
      do not check explain extended/explain extended_noaddr */
  } else if (0 == sql_ctx->first_plan_hash_) {  /* generate plan first time */
    void *tmp_ptr = NULL;
    sql_ctx->first_outline_data_.reset();
    if (OB_UNLIKELY(0 == real_plan->get_signature()) || 0 < sql_ctx->retry_times_) {
      /* do nothing */
    } else if (OB_SUCC(OB_E(EventTable::EN_EXPLAIN_GENERATE_PLAN_WITH_OUTLINE) OB_SUCCESS)) {
      /* do nothing */
    } else if (OB_UNLIKELY(NULL == (tmp_ptr = get_optimizer_context().get_allocator().alloc(OB_MAX_SQL_LENGTH)))) {
      /* allocator in optimizer context is from ObResultSet */
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_ERROR("fail to alloc memory", K(ret));
    } else {
      PlanText plan_text;
      plan_text.buf_ = static_cast<char *>(tmp_ptr);
      plan_text.buf_len_ = OB_MAX_SQL_LENGTH;
      if (OB_FAIL(ObSqlPlan::get_plan_outline_info_one_line(plan_text, real_plan))) {
      } else {
        sql_ctx->first_outline_data_.assign_ptr(plan_text.buf_, static_cast<ObString::obstr_size_t>(plan_text.pos_));
        sql_ctx->first_plan_hash_ = real_plan->get_signature();
        sql_ctx->first_equal_param_cons_cnt_ = query_ctx->all_equal_param_constraints_.count();
        sql_ctx->first_const_param_cons_cnt_ = query_ctx->all_plan_const_param_constraints_.count();
        sql_ctx->first_expr_cons_cnt_ = query_ctx->all_expr_constraints_.count();
        ret = OB_SQL_RETRY_OUTLINE;
      }
    }
  } else {  /* check generate plan again use outline data */
    SMART_VAR(char[OB_MAX_SQL_LENGTH], buf) {
      PlanText plan_text;
      plan_text.buf_ = buf;
      plan_text.buf_len_ = OB_MAX_SQL_LENGTH;
      if (OB_FAIL(ObSqlPlan::get_plan_outline_info_one_line(plan_text, real_plan))) {
      } else {
        ObString cur_outline_data;
        const uint64_t cur_plan_hash = real_plan->get_signature();
        cur_outline_data.assign_ptr(plan_text.buf_, static_cast<ObString::obstr_size_t>(plan_text.pos_));
        if (cur_plan_hash != sql_ctx->first_plan_hash_) {
          ret = OB_OUTLINE_NOT_REPRODUCIBLE;
          LOG_WARN("failed to generate plan use outline", K(sql_ctx->first_plan_hash_));
        }
        if (0 != (cur_outline_data.case_compare(sql_ctx->first_outline_data_))) {
          ret = OB_OUTLINE_NOT_REPRODUCIBLE;
          LOG_WARN("failed to generate plan use outline", K(sql_ctx->first_outline_data_));
        }
        if (query_ctx->all_equal_param_constraints_.count() != sql_ctx->first_equal_param_cons_cnt_ || 
            query_ctx->all_plan_const_param_constraints_.count() != sql_ctx->first_const_param_cons_cnt_ || 
            query_ctx->all_expr_constraints_.count() != sql_ctx->first_expr_cons_cnt_) {
          ret = OB_OUTLINE_NOT_REPRODUCIBLE;
          LOG_WARN("failed to generate plan use outline", K(sql_ctx->first_equal_param_cons_cnt_), 
                                                          K(sql_ctx->first_const_param_cons_cnt_), 
                                                          K(sql_ctx->first_expr_cons_cnt_),
                                                          K(query_ctx->all_equal_param_constraints_.count()),
                                                          K(query_ctx->all_plan_const_param_constraints_.count()),
                                                          K(query_ctx->all_expr_constraints_.count()));
        }
        if (OB_FAIL(ret)) {
        }
      }
      sql_ctx->first_plan_hash_ = 0;
      sql_ctx->first_outline_data_.reset();
      sql_ctx->first_equal_param_cons_cnt_ = 0;
      sql_ctx->first_const_param_cons_cnt_ = 0;
      sql_ctx->first_expr_cons_cnt_ = 0;
    }
  }
  return ret;
}
