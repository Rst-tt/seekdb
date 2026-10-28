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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "common/timezone/ob_time_convert.h"
#include <glob.h>


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_,
                    has_correlated_expr_, is_file_scan_, file_format_,
                    file_header_, file_delimiter_, file_quote_);

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (MY_SPEC.is_file_scan_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_file_scan;
    ObDatum *path_datum = nullptr;
    if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, path_datum))) {
    } else if (OB_ISNULL(path_datum) || path_datum->is_null()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ObFileSqlOptions options;
      options.header_ = MY_SPEC.file_header_ != 0;
      options.delimiter_ = static_cast<char>(MY_SPEC.file_delimiter_);
      options.quote_ = static_cast<char>(MY_SPEC.file_quote_);
      if (static_cast<int64_t>(ObFileSqlFormat::CSV) == MY_SPEC.file_format_) {
        ret = csv_reader_.open(path_datum->get_string(), options);
      } else if (static_cast<int64_t>(ObFileSqlFormat::JSONL) == MY_SPEC.file_format_ ||
                 static_cast<int64_t>(ObFileSqlFormat::NDJSON) == MY_SPEC.file_format_) {
        ret = jsonl_reader_.open(path_datum->get_string(), options);
      } else if (static_cast<int64_t>(ObFileSqlFormat::PARQUET) == MY_SPEC.file_format_) {
        ret = parquet_reader_.open(path_datum->get_string(), options);
      } else if (static_cast<int64_t>(ObFileSqlFormat::GLOB) == MY_SPEC.file_format_) {
        glob_files_.clear();
        glob_idx_ = 0;
        glob_t glob_result;
        memset(&glob_result, 0, sizeof(glob_result));
        std::string pattern(path_datum->get_string().ptr(),
                            path_datum->get_string().length());
        int glob_ret = glob(pattern.c_str(), 0, nullptr, &glob_result);
        if (glob_ret != 0 && GLOB_NOMATCH != glob_ret) {
          ret = OB_FILE_NOT_EXIST;
        } else {
          for (size_t i = 0; i < glob_result.gl_pathc; ++i) {
            glob_files_.push_back(std::string(glob_result.gl_pathv[i]));
          }
        }
        if (glob_result.gl_pathv != nullptr) {
          globfree(&glob_result);
        }
      } else {
        ret = OB_NOT_SUPPORTED;
      }
    }
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    node_idx_ = 0;
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
    if (OB_SUCC(ret) && MY_SPEC.is_file_scan_) {
      if (static_cast<int64_t>(ObFileSqlFormat::CSV) == MY_SPEC.file_format_) {
        ret = csv_reader_.reset();
      } else if (static_cast<int64_t>(ObFileSqlFormat::JSONL) == MY_SPEC.file_format_ ||
                 static_cast<int64_t>(ObFileSqlFormat::NDJSON) == MY_SPEC.file_format_) {
        ret = jsonl_reader_.reset();
      } else if (static_cast<int64_t>(ObFileSqlFormat::PARQUET) == MY_SPEC.file_format_) {
        ret = parquet_reader_.reset();
      } else if (static_cast<int64_t>(ObFileSqlFormat::GLOB) == MY_SPEC.file_format_) {
        glob_idx_ = 0;
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  return ret;
}

void ObFunctionTableOp::destroy()
{
  ObOperator::destroy();
}


int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_file_scan()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (OB_FAIL(ctx_.check_status())) {
  } else if (static_cast<int64_t>(ObFileSqlFormat::GLOB) == MY_SPEC.file_format_) {
    if (glob_idx_ >= glob_files_.size()) {
      ret = OB_ITER_END;
    } else if (MY_SPEC.column_exprs_.count() < 1) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObExpr *column_expr = MY_SPEC.column_exprs_.at(0);
      ObDatum &datum = column_expr->locate_datum_for_write(eval_ctx_);
      const std::string &file = glob_files_[glob_idx_++];
      datum.set_string(file.data(), static_cast<uint32_t>(file.size()));
      column_expr->set_evaluated_projected(eval_ctx_);
    }
  } else {
    std::vector<std::string> *record = nullptr;
    std::vector<bool> *nulls = nullptr;
    if (static_cast<int64_t>(ObFileSqlFormat::CSV) == MY_SPEC.file_format_) {
      csv_record_.clear();
      csv_nulls_.clear();
      record = &csv_record_;
      nulls = &csv_nulls_;
      if (OB_FAIL(csv_reader_.next(csv_record_, csv_nulls_))) {
        if (OB_ITER_END != ret) {
        }
      }
    } else if (static_cast<int64_t>(ObFileSqlFormat::JSONL) == MY_SPEC.file_format_ ||
               static_cast<int64_t>(ObFileSqlFormat::NDJSON) == MY_SPEC.file_format_) {
      jsonl_record_.clear();
      jsonl_nulls_.clear();
      record = &jsonl_record_;
      nulls = &jsonl_nulls_;
      if (OB_FAIL(jsonl_reader_.next(jsonl_record_, jsonl_nulls_))) {
        if (OB_ITER_END != ret) {
        }
      }
    } else if (static_cast<int64_t>(ObFileSqlFormat::PARQUET) == MY_SPEC.file_format_) {
      parquet_record_.clear();
      parquet_nulls_.clear();
      record = &parquet_record_;
      nulls = &parquet_nulls_;
      if (OB_FAIL(parquet_reader_.next(parquet_record_, parquet_nulls_))) {
        if (OB_ITER_END != ret) {
        }
      }
    } else {
      ret = OB_NOT_SUPPORTED;
    }
    if (OB_FAIL(ret)) {
      if (OB_ITER_END == ret) {
        // no more rows
      }
    } else if (OB_ISNULL(record) ||
               record->size() != static_cast<size_t>(MY_SPEC.column_exprs_.count())) {
      ret = OB_INVALID_DATA;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
        ObExpr *column_expr = MY_SPEC.column_exprs_.at(i);
        ObDatum &datum = column_expr->locate_datum_for_write(eval_ctx_);
        const std::string &field = record->at(i);
        const ObObjType type = column_expr->datum_meta_.type_;
        if (OB_NOT_NULL(nulls) && nulls->at(i)) {
          datum.set_null();
        } else if (ObIntType == type) {
          datum.set_int(atoll(field.c_str()));
        } else if (ObDoubleType == type) {
          datum.set_double(atof(field.c_str()));
        } else if (ObFloatType == type) {
          datum.set_float(static_cast<float>(atof(field.c_str())));
        } else if (ObTinyIntType == type) {
          datum.set_bool(field == "true" || field == "TRUE");
        } else if (ObDateType == type) {
          int32_t date_value = 0;
          ObString field_str(static_cast<int32_t>(field.size()), field.data());
          if (OB_FAIL(ObTimeConverter::str_to_date(field_str, date_value))) {
          } else {
            datum.set_int32(date_value);
          }
        } else if (ObDateTimeType == type) {
          int64_t datetime_value = 0;
          ObString field_str(static_cast<int32_t>(field.size()), field.data());
          ObTimeConvertCtx cvrt_ctx(nullptr, false);
          if (OB_FAIL(ObTimeConverter::str_to_datetime(field_str, cvrt_ctx, datetime_value))) {
          } else {
            datum.set_int(datetime_value);
          }
        } else if (ob_is_string_tc(type) || ob_is_text_tc(type)) {
          datum.set_string(field.data(), static_cast<uint32_t>(field.size()));
        } else {
          ret = OB_NOT_SUPPORTED;
        }
        if (OB_SUCC(ret)) {
          column_expr->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
