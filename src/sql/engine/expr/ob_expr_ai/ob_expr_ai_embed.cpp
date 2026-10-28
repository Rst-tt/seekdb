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
#include "ob_expr_ai_embed.h"
#include <algorithm>
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "sql/engine/expr/ob_datum_cast.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

// one request may carry several inputs, the upper bound keeps a single request
// and the temporary buffer it needs within a reasonable size.
static const int64_t AI_EMBED_MAX_INPUT_PER_REQUEST = 64;

namespace
{

struct ObAIEmbedRowItem
{
  int64_t row_idx_;
  common::ObString model_id_;
  int64_t dim_;
};

struct ObAIEmbedRowItemLess
{
  bool operator()(const ObAIEmbedRowItem &lhs, const ObAIEmbedRowItem &rhs) const
  {
    bool less = false;
    if (lhs.dim_ != rhs.dim_) {
      less = lhs.dim_ < rhs.dim_;
    } else {
      less = lhs.model_id_.compare(rhs.model_id_) < 0;
    }
    return less;
  }
};

bool is_same_embed_group(const ObAIEmbedRowItem &lhs, const ObAIEmbedRowItem &rhs)
{
  return lhs.dim_ == rhs.dim_ && 0 == lhs.model_id_.compare(rhs.model_id_);
}

}  // namespace

ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

// build the config which carries the explicit dimension of the embedding
static int build_embed_config(ObIAllocator &allocator, int64_t dim, ObJsonObject *&config)
{
  int ret = OB_SUCCESS;
  ObJsonInt *dim_json = nullptr;
  if (dim > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dim, dim_json))) {
    } else if (OB_FAIL(config->add("dimensions", dim_json))) {
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
    res.set_null();
  } else if (arg_content->is_null()) {
    // a null content has no embedding, the result is null as well
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content = arg_content->get_string();
    if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim) && !arg_dim->is_null()) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(build_embed_config(temp_allocator, dim, config))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

// vectorized evaluation: every request carries several rows, the embeddings are
// mapped back to the rows they were computed for, null inputs stay null.
int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip, const int64_t size)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(size <= 0)) {
  } else if (OB_FAIL(expr.args_[MODEL_IDX]->eval_batch(ctx, skip, size))) {
  } else if (OB_FAIL(expr.args_[CONTENT_IDX]->eval_batch(ctx, skip, size))) {
  } else if (expr.arg_cnt_ > DIM_IDX && OB_FAIL(expr.args_[DIM_IDX]->eval_batch(ctx, skip, size))) {
  } else {
    ObDatumVector res_datums = expr.locate_expr_datumvector(ctx);
    ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
    ObDatumVector model_datums = expr.args_[MODEL_IDX]->locate_expr_datumvector(ctx);
    ObDatumVector content_datums = expr.args_[CONTENT_IDX]->locate_expr_datumvector(ctx);
    ObDatumVector dim_datums;
    if (expr.arg_cnt_ > DIM_IDX) {
      dim_datums = expr.args_[DIM_IDX]->locate_expr_datumvector(ctx);
    }
    if (OB_ISNULL(res_datums.at(0)) || OB_ISNULL(model_datums.at(0)) || OB_ISNULL(content_datums.at(0))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null datum vector", K(ret));
    } else {
      ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
      common::ObArenaAllocator &tmp_allocator = tmp_alloc_g.get_allocator();
      int64_t need_cnt = 0;
      for (int64_t j = 0; j < size; ++j) {
        if (!skip.at(j) && !eval_flags.at(j)) {
          ++need_cnt;
        }
      }
      ObAIEmbedRowItem *items = nullptr;
      if (0 == need_cnt) {
        // all the rows of this batch are already evaluated
      } else if (OB_ISNULL(items = static_cast<ObAIEmbedRowItem *>(
                               tmp_allocator.alloc(sizeof(ObAIEmbedRowItem) * need_cnt)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        int64_t item_cnt = 0;
        for (int64_t j = 0; OB_SUCC(ret) && j < size; ++j) {
          if (skip.at(j) || eval_flags.at(j)) {
            continue;
          }
          ObDatum *model_datum = model_datums.at(j);
          ObDatum *content_datum = content_datums.at(j);
          if (OB_ISNULL(model_datum) || OB_ISNULL(content_datum)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (model_datum->is_null()) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
          } else if (content_datum->is_null()) {
            // null input has no embedding, keep the null result of this row
            res_datums.at(j)->set_null();
            eval_flags.set(j);
          } else if (model_datum->get_string().empty() || content_datum->get_string().empty()) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
          } else {
            int64_t dim = 0;
            if (expr.arg_cnt_ > DIM_IDX && OB_NOT_NULL(dim_datums.at(j)) && !dim_datums.at(j)->is_null()) {
              dim = dim_datums.at(j)->get_int();
            }
            if (expr.arg_cnt_ > DIM_IDX && OB_NOT_NULL(dim_datums.at(j))
                && !dim_datums.at(j)->is_null() && dim <= 0) {
              ret = OB_INVALID_ARGUMENT;
              LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
            } else {
              items[item_cnt].row_idx_ = j;
              items[item_cnt].model_id_ = model_datum->get_string();
              items[item_cnt].dim_ = dim;
              ++item_cnt;
            }
          }
        }
        if (OB_SUCC(ret) && item_cnt > 0) {
          std::sort(items, items + item_cnt, ObAIEmbedRowItemLess());
          ObString cur_model;
          int64_t cur_dim = -1;
          ObAIFuncExprInfo *info = nullptr;
          share::ObAiModelEndpointInfo resolved_endpoint;
          query::ObIAiEndpointResolver *endpoint_resolver =
              ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
          for (int64_t g_start = 0; OB_SUCC(ret) && g_start < item_cnt; ) {
            int64_t g_end = g_start + 1;
            while (g_end < item_cnt && is_same_embed_group(items[g_start], items[g_end])) {
              ++g_end;
            }
            const ObString model_id = items[g_start].model_id_;
            const int64_t dim = items[g_start].dim_;
            ObJsonObject *config = nullptr;
            info = nullptr;
            resolved_endpoint.reset();
            if (OB_ISNULL(endpoint_resolver)) {
              ret = OB_ERR_UNEXPECTED;
            } else if (OB_FAIL(build_embed_config(tmp_allocator, dim, config))) {
            } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(tmp_allocator, model_id, info))) {
            } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                           model_id, tmp_allocator, resolved_endpoint))) {
            } else {
              ObAIFuncModel model(tmp_allocator, *info, resolved_endpoint);
              for (int64_t c_start = g_start; OB_SUCC(ret) && c_start < g_end; ) {
                const int64_t c_end = std::min(g_end, c_start + AI_EMBED_MAX_INPUT_PER_REQUEST);
                ObArray<ObString> contents;
                ObArray<ObString> results;
                if (OB_FAIL(contents.reserve(static_cast<int64_t>(c_end - c_start)))) {
                } else if (OB_FAIL(results.reserve(static_cast<int64_t>(c_end - c_start)))) {
                }
                for (int64_t k = c_start; OB_SUCC(ret) && k < c_end; ++k) {
                  const int64_t row_idx = items[k].row_idx_;
                  ObString content;
                  if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                          ctx.exec_ctx_, tmp_allocator, *content_datums.at(row_idx),
                          expr.args_[CONTENT_IDX]->datum_meta_,
                          expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content))) {
                  } else if (OB_FAIL(contents.push_back(content))) {
                  }
                }
                if (OB_FAIL(ret)) {
                } else if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, results))) {
                } else if (results.count() != contents.count()) {
                  ret = OB_ERR_UNEXPECTED;
                  LOG_WARN("unexpected embedding result count", K(ret), K(results.count()), K(contents.count()));
                }
                for (int64_t k = c_start; OB_SUCC(ret) && k < c_end; ++k) {
                  const int64_t row_idx = items[k].row_idx_;
                  const ObString &res_str = results.at(k - c_start);
                  char *res_buf = nullptr;
                  int64_t res_buf_len = 0;
                  ObTextStringDatumResult output_result(expr.datum_meta_.type_, &expr, &ctx,
                                                        res_datums.at(row_idx));
                  if (OB_FAIL(output_result.init_with_batch_idx(res_str.length(), row_idx))) {
                  } else if (OB_FAIL(output_result.get_reserved_buffer(res_buf, res_buf_len))) {
                  } else if (res_buf_len < res_str.length()) {
                    ret = OB_ERR_UNEXPECTED;
                  } else if (OB_FAIL(output_result.lseek(res_str.length(), 0))) {
                  } else {
                    if (res_str.length() > 0) {
                      MEMCPY(res_buf, res_str.ptr(), res_str.length());
                    }
                    output_result.set_result();
                    eval_flags.set(row_idx);
                  }
                }
                c_start = c_end;
              }
            }
            g_start = g_end;
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  int ret = OB_SUCCESS;
  rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
  rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  return ret;
}

} // namespace sql
} // namespace oceanbase
