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
#include "ob_expr_ai_rerank.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
#include <algorithm>
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

namespace {

struct ObRerankResultItem
{
  ObJsonObject *obj_;
  int64_t index_;
  double score_;
  ObRerankResultItem() : obj_(nullptr), index_(0), score_(0.0) {}
};

struct ObRerankResultItemLess
{
  bool operator()(const ObRerankResultItem &lhs, const ObRerankResultItem &rhs) const
  {
    if (lhs.score_ != rhs.score_) {
      return lhs.score_ > rhs.score_;
    }
    return lhs.index_ < rhs.index_;
  }
};

} // namespace

int ObExprAIRerank::check_documents_type(ObJsonArray *document_array, bool has_doc_key)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(document_array)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("document array is null", K(ret));
  } else {
    const int64_t count = document_array->element_count();
    for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
      ObJsonNode *doc_node = document_array->get_value(i);
      if (OB_ISNULL(doc_node)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("document node is null", K(ret), K(i));
      } else if (has_doc_key && doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
      } else if (!has_doc_key && doc_node->json_type() != ObJsonNodeType::J_STRING) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
      }
    }
  }
  return ret;
}

int ObExprAIRerank::get_rerank_result_index(ObJsonObject *result_obj, int64_t &index)
{
  int ret = OB_SUCCESS;
  ObJsonNode *index_node = nullptr;
  index = 0;
  if (OB_ISNULL(result_obj)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(index_node = result_obj->get_value(ObString(INDEX_KEY)))) {
    ret = OB_INVALID_DATA;
    LOG_WARN("ai_rerank result has no index", K(ret));
  } else if (index_node->json_type() == ObJsonNodeType::J_INT) {
    index = static_cast<ObJsonInt *>(index_node)->value();
  } else if (index_node->json_type() == ObJsonNodeType::J_UINT) {
    const uint64_t uint_value = static_cast<ObJsonUint *>(index_node)->value();
    if (uint_value > static_cast<uint64_t>(INT64_MAX)) {
      ret = OB_INVALID_DATA;
      LOG_WARN("ai_rerank result index overflow", K(ret), K(uint_value));
    } else {
      index = static_cast<int64_t>(uint_value);
    }
  } else {
    ret = OB_INVALID_DATA;
    LOG_WARN("ai_rerank result index is not integer", K(ret));
  }
  return ret;
}

int ObExprAIRerank::get_rerank_result_score(ObJsonObject *result_obj, double &score)
{
  int ret = OB_SUCCESS;
  ObJsonNode *score_node = nullptr;
  score = 0.0;
  if (OB_ISNULL(result_obj)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(score_node = result_obj->get_value(ObString(SCORE_KEY)))) {
    ret = OB_INVALID_DATA;
    LOG_WARN("ai_rerank result has no score", K(ret));
  } else if (score_node->json_type() == ObJsonNodeType::J_DOUBLE) {
    score = static_cast<ObJsonDouble *>(score_node)->value();
  } else if (score_node->json_type() == ObJsonNodeType::J_INT) {
    score = static_cast<double>(static_cast<ObJsonInt *>(score_node)->value());
  } else if (score_node->json_type() == ObJsonNodeType::J_UINT) {
    score = static_cast<double>(static_cast<ObJsonUint *>(score_node)->value());
  } else {
    ret = OB_INVALID_DATA;
    LOG_WARN("ai_rerank result score is not number", K(ret));
  }
  return ret;
}

int ObExprAIRerank::sort_rerank_result_array(ObIAllocator &allocator,
                                             ObJsonArray *result_array,
                                             ObJsonArray *&sorted_array)
{
  int ret = OB_SUCCESS;
  ObJsonArray *sorted_result_array = nullptr;
  if (OB_ISNULL(result_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, sorted_result_array))) {
    LOG_WARN("failed to create json array", K(ret));
  } else {
    const int64_t count = result_array->element_count();
    if (count > 0) {
      ObRerankResultItem *items = static_cast<ObRerankResultItem *>(
          allocator.alloc(sizeof(ObRerankResultItem) * count));
      if (OB_ISNULL(items)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to allocate sort items", K(ret), K(count));
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        items[i].obj_ = nullptr;
        items[i].index_ = 0;
        items[i].score_ = 0.0;
        ObJsonNode *node = result_array->get_value(i);
        if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_DATA;
          LOG_WARN("ai_rerank result element is not object", K(ret), K(i));
        } else if (OB_FAIL(get_rerank_result_index(static_cast<ObJsonObject *>(node), items[i].index_))) {
          LOG_WARN("ai_rerank result index is invalid", K(ret), K(i));
        } else if (OB_FAIL(get_rerank_result_score(static_cast<ObJsonObject *>(node), items[i].score_))) {
          LOG_WARN("ai_rerank result score is invalid", K(ret), K(i));
        } else {
          items[i].obj_ = static_cast<ObJsonObject *>(node);
        }
      }
      if (OB_SUCC(ret)) {
        std::sort(items, items + count, ObRerankResultItemLess());
        for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
          if (OB_FAIL(sorted_result_array->append(items[i].obj_))) {
            LOG_WARN("failed to append sorted result", K(ret), K(i));
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      sorted_array = sorted_result_array;
    }
  }
  return ret;
}

int ObExprAIRerank::call_rerank_in_batches(ObIAllocator &allocator,
                                           const ObAIFuncExprInfo &info,
                                           const share::ObAiModelEndpointInfo &endpoint_info,
                                           ObArray<ObString> &header_array,
                                           ObString &query,
                                           ObJsonArray *text_array,
                                           ObJsonArray *&result_array)
{
  int ret = OB_SUCCESS;
  const int64_t batch_size = 20; // max batch size for rerank
  ObJsonArray *merged_array = nullptr;
  ObString score_key(SCORE_KEY);
  if (OB_ISNULL(text_array)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("text array is null", K(ret));
  } else if (text_array->element_count() == 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, merged_array))) {
    LOG_WARN("failed to create json array", K(ret));
  } else {
    const int64_t count = text_array->element_count();
    for (int64_t start = 0; OB_SUCC(ret) && start < count; start += batch_size) {
      const int64_t end = (start + batch_size < count) ? (start + batch_size) : count;
      const int64_t batch_count = end - start;
      ObJsonArray *batch_document_array = nullptr;
      ObJsonArray *batch_result_array = nullptr;
      ObJsonArray *sorted_result_array = nullptr;
      ObJsonArray *next_merged_array = nullptr;
      if (OB_FAIL(construct_batch_document_array(allocator, text_array, start, end,
                                                 batch_document_array))) {
        LOG_WARN("failed to construct batch document array", K(ret), K(start), K(end));
      } else if (OB_FAIL(inner_eval_ai_rerank(allocator, info, endpoint_info, header_array,
                                              query, batch_document_array, batch_result_array))) {
        LOG_WARN("failed to eval ai_rerank batch", K(ret), K(start), K(end));
      } else if (OB_FAIL(batch_result_add_base(allocator, batch_result_array, start, batch_count))) {
        LOG_WARN("failed to add batch base index", K(ret), K(start), K(batch_count));
      } else if (OB_FAIL(sort_rerank_result_array(allocator, batch_result_array, sorted_result_array))) {
        LOG_WARN("failed to sort rerank result", K(ret), K(start));
      } else if (OB_FAIL(compact_json_array_by_key(allocator, merged_array, sorted_result_array,
                                                   score_key, next_merged_array))) {
        LOG_WARN("failed to merge rerank result", K(ret), K(start));
      } else {
        merged_array = next_merged_array;
      }
    }
    if (OB_SUCC(ret)) {
      result_array = merged_array;
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
    LOG_WARN("failed to eval ai_rerank params", K(ret));
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    ObArray<ObString> header_array;
    ObJsonArray *document_array = nullptr;
    ObJsonArray *text_array = nullptr;
    ObJsonArray *result_array = nullptr;
    ObJsonArray *output_array = nullptr;
    ObIJsonBase *j_base = nullptr;
    bool is_null_result = false;
    const bool has_doc_key = (nullptr != arg_doc_key && !arg_doc_key->is_null());
    ObString doc_key;
    if (has_doc_key) {
      doc_key = arg_doc_key->get_string();
    }
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
                                               j_base, is_null_result))) {
      LOG_WARN("failed to get documents json", K(ret));
    } else if (is_null_result || OB_ISNULL(j_base)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is null");
      res.set_null();
    } else if (j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (OB_ISNULL(document_array = static_cast<ObJsonArray *>(j_base))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (document_array->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
    } else if (has_doc_key && doc_key.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key is empty");
    } else if (OB_FAIL(check_documents_type(document_array, has_doc_key))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
      LOG_WARN("failed to get ai func info", K(ret), K(model_id));
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(model_id, temp_allocator,
                                                                resolved_endpoint))) {
      LOG_WARN("failed to resolve endpoint", K(ret), K(model_id));
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
      LOG_WARN("failed to check ai func type", K(ret));
    } else if (OB_FAIL(ObAIFuncUtils::get_header(temp_allocator, *info, *endpoint_info,
                                                 header_array))) {
      LOG_WARN("failed to get header", K(ret));
    } else if (has_doc_key
               && OB_FAIL(get_doc_array_from_documents_array_with_key(temp_allocator, document_array,
                                                                     doc_key, text_array))) {
      LOG_WARN("failed to get doc array with key", K(ret), K(doc_key));
    } else {
      if (!has_doc_key) {
        text_array = document_array;
      }
      if (OB_FAIL(call_rerank_in_batches(temp_allocator, *info, *endpoint_info, header_array,
                                         query, text_array, result_array))) {
        LOG_WARN("failed to call rerank in batches", K(ret));
      } else if (!has_doc_key) {
        output_array = result_array;
      } else if (OB_FAIL(sort_document_array_by_model_result(temp_allocator, document_array,
                                                             result_array, output_array))) {
        LOG_WARN("failed to sort document array", K(ret));
      }
      if (OB_SUCC(ret)) {
        ObString raw_str;
        if (OB_FAIL(output_array->get_raw_binary(raw_str, &temp_allocator))) {
          LOG_WARN("failed to get raw binary", K(ret));
        } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
          LOG_WARN("failed to pack json str res", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::get_doc_array_from_documents_array_with_key(ObIAllocator &allocator, ObJsonArray *document_object_array, ObString &doc_key, ObJsonArray *&doc_array)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(document_object_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (doc_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonArray *doc_array_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, doc_array_obj))) {
    } else {
      int64_t count = document_object_array->element_count();
      ObJsonNode *doc_node = nullptr;
      ObJsonNode *doc_value_node = nullptr;
      ObJsonObject *doc_obj = nullptr;
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        doc_node = document_object_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is null");
        } else if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else {
          doc_obj = static_cast<ObJsonObject *>(doc_node);
          doc_value_node = doc_obj->get_value(doc_key);
          if (OB_ISNULL(doc_value_node)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
          } else if (doc_value_node->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
          } else if (OB_FAIL(doc_array_obj->append(doc_value_node))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      doc_array = doc_array_obj;
    }
  }
  return ret;
}

int ObExprAIRerank::sort_document_array_by_model_result(ObIAllocator &allocator,
                                                          ObJsonArray *document_array,
                                                          ObJsonArray *model_result_array,
                                                          ObJsonArray *&sorted_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *sorted_document_array_obj = nullptr;
  if (OB_ISNULL(document_array) || OB_ISNULL(model_result_array)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("input array is null", K(ret));
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, sorted_document_array_obj))) {
    LOG_WARN("failed to create json array", K(ret));
  } else {
    const int64_t doc_count = document_array->element_count();
    const int64_t result_count = model_result_array->element_count();
    for (int64_t i = 0; OB_SUCC(ret) && i < result_count; i++) {
      ObJsonNode *result_node = model_result_array->get_value(i);
      ObJsonNode *origin_node = nullptr;
      ObJsonObject *new_obj = nullptr;
      int64_t index = 0;
      double score = 0.0;
      if (OB_ISNULL(result_node) || result_node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
        LOG_WARN("ai_rerank result element is not object", K(ret), K(i));
      } else if (OB_FAIL(get_rerank_result_index(static_cast<ObJsonObject *>(result_node), index))) {
        LOG_WARN("ai_rerank result index is invalid", K(ret), K(i));
      } else if (OB_FAIL(get_rerank_result_score(static_cast<ObJsonObject *>(result_node), score))) {
        LOG_WARN("ai_rerank result score is invalid", K(ret), K(i));
      } else if (index < 0 || index >= doc_count) {
        ret = OB_INVALID_DATA;
        LOG_WARN("ai_rerank result index out of range", K(ret), K(index), K(doc_count));
      } else if (OB_ISNULL(origin_node = document_array->get_value(index))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (origin_node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
        LOG_WARN("ai_rerank document is not object", K(ret), K(index));
      } else if (OB_ISNULL(new_obj = static_cast<ObJsonObject *>(
                     static_cast<ObJsonObject *>(origin_node)->clone(&allocator, true)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to clone document object", K(ret));
      } else {
        ObJsonDouble *score_value = nullptr;
        if (OB_ISNULL(score_value = OB_NEWx(ObJsonDouble, &allocator, score))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("failed to allocate score json", K(ret));
        } else if (OB_FAIL(new_obj->add(MODEL_SCORE_KEY, score_value))) {
          LOG_WARN("failed to add model score", K(ret));
        } else if (OB_FAIL(sorted_document_array_obj->append(new_obj))) {
          LOG_WARN("failed to append document", K(ret));
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    sorted_document_array = sorted_document_array_obj;
  }
  return ret;
}

int ObExprAIRerank::construct_batch_document_array(ObIAllocator &allocator, ObJsonArray *document_array, int64_t start_idx, int64_t end_idx, ObJsonArray *&batch_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = start_idx; i < end_idx; i++) {
      if (OB_FAIL(result_array->append(document_array->get_value(i)))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    batch_document_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::inner_eval_ai_rerank(ObIAllocator &allocator, 
                                          const ObAIFuncExprInfo &info, 
                                          const share::ObAiModelEndpointInfo &endpoint_info,
                                          ObArray<ObString> &header_array, 
                                          ObString &query, 
                                          ObJsonArray *document_array, 
                                          ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  ObJsonArray *res = nullptr;
  ObJsonObject *config_json = nullptr;
  ObJsonObject *body = nullptr;
  ObJsonObject *http_response = nullptr;
  ObIJsonBase *response = nullptr;
  ObAIFuncClient ai_client;
  if (OB_FAIL(ObAIFuncUtils::get_rerank_body(allocator, info, endpoint_info, query, document_array, config_json, body))) {
  } else if (OB_FAIL(ai_client.send_post(allocator, endpoint_info.get_url(), header_array, body, http_response))) {
  } else if (OB_FAIL(ObAIFuncUtils::parse_rerank_output(allocator, endpoint_info, http_response, response))) {
  } else if (response->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(res = static_cast<ObJsonArray *>(response))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    result_array = res;
  }
  return ret;
}

int ObExprAIRerank::batch_result_add_base(ObIAllocator &allocator, ObJsonArray *array,
                                          int64_t start_idx, int64_t batch_count)
{
  int ret = OB_SUCCESS;
  UNUSED(allocator);
  if (OB_ISNULL(array)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const int64_t count = array->element_count();
    if (count <= 0 || count > batch_count) {
      ret = OB_INVALID_DATA;
      LOG_WARN("ai_rerank batch result count is invalid", K(ret), K(count), K(batch_count));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
      ObJsonNode *node = array->get_value(i);
      int64_t index = 0;
      if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
        LOG_WARN("ai_rerank batch result is not object", K(ret), K(i));
      } else if (OB_FAIL(get_rerank_result_index(static_cast<ObJsonObject *>(node), index))) {
        LOG_WARN("ai_rerank batch result index is invalid", K(ret), K(i));
      } else if (index < 0 || index >= batch_count) {
        ret = OB_INVALID_DATA;
        LOG_WARN("ai_rerank batch result index out of range", K(ret), K(index), K(batch_count));
      } else {
        ObJsonNode *index_node = static_cast<ObJsonObject *>(node)->get_value(ObString(INDEX_KEY));
        const int64_t global_index = index + start_idx;
        if (OB_ISNULL(index_node)) {
          ret = OB_INVALID_DATA;
        } else if (index_node->json_type() == ObJsonNodeType::J_INT) {
          static_cast<ObJsonInt *>(index_node)->set_value(global_index);
        } else {
          static_cast<ObJsonUint *>(index_node)->set_value(static_cast<uint64_t>(global_index));
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::compact_json_array_by_key(ObIAllocator &allocator, ObJsonArray *array1,
                                                ObJsonArray *array2, ObString &score_key,
                                                ObJsonArray *&compact_array)
{
  int ret = OB_SUCCESS;
  UNUSED(score_key);
  ObJsonArray *result_array = nullptr;
  if (OB_ISNULL(array1) || OB_ISNULL(array2)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
    LOG_WARN("failed to create json array", K(ret));
  } else {
    const int64_t count1 = array1->element_count();
    const int64_t count2 = array2->element_count();
    int64_t i = 0;
    int64_t j = 0;
    while (OB_SUCC(ret) && i < count1 && j < count2) {
      ObJsonNode *node1 = array1->get_value(i);
      ObJsonNode *node2 = array2->get_value(j);
      double score1 = 0.0;
      double score2 = 0.0;
      int64_t index1 = 0;
      int64_t index2 = 0;
      if (OB_ISNULL(node1) || node1->json_type() != ObJsonNodeType::J_OBJECT
          || OB_ISNULL(node2) || node2->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
        LOG_WARN("ai_rerank result element is not object", K(ret), K(i), K(j));
      } else if (OB_FAIL(get_rerank_result_score(static_cast<ObJsonObject *>(node1), score1))
                 || OB_FAIL(get_rerank_result_score(static_cast<ObJsonObject *>(node2), score2))) {
        LOG_WARN("failed to get rerank score", K(ret), K(i), K(j));
      } else if (OB_FAIL(get_rerank_result_index(static_cast<ObJsonObject *>(node1), index1))
                 || OB_FAIL(get_rerank_result_index(static_cast<ObJsonObject *>(node2), index2))) {
        LOG_WARN("failed to get rerank index", K(ret), K(i), K(j));
      } else if (score1 > score2 || (score1 == score2 && index1 <= index2)) {
        if (OB_FAIL(result_array->append(node1))) {
          LOG_WARN("failed to append json node", K(ret));
        }
        i++;
      } else {
        if (OB_FAIL(result_array->append(node2))) {
          LOG_WARN("failed to append json node", K(ret));
        }
        j++;
      }
    }
    if (OB_SUCC(ret)) {
      while (i < count1) {
        if (OB_FAIL(result_array->append(array1->get_value(i)))) {
          LOG_WARN("failed to append json node", K(ret));
        }
        i++;
      }
    }
    if (OB_SUCC(ret)) {
      while (j < count2) {
        if (OB_FAIL(result_array->append(array2->get_value(j)))) {
          LOG_WARN("failed to append json node", K(ret));
        }
        j++;
      }
    }
  }
  if (OB_SUCC(ret)) {
    compact_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::construct_config_json(ObIAllocator &allocator, int64_t top_n, int64_t return_doc, ObJsonObject *&config_json)
{
  INIT_SUCC(ret);
  ObJsonObject *config_obj = nullptr;
  ObJsonInt *top_n_obj = nullptr;
  ObJsonBoolean *return_doc_obj = nullptr;
  bool return_doc_value = true;
  if (return_doc <= 0) {
    return_doc_value = false;
  }
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config_obj))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_boolean(allocator, return_doc_value, return_doc_obj))) {
  } else if (OB_FAIL(config_obj->add("return_documents", return_doc_obj))) {
  } else if (top_n > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, top_n, top_n_obj))) {
    } else if (OB_FAIL(config_obj->add("top_n", top_n_obj))) {
    }
  }
  if (OB_SUCC(ret)) {
    config_json = config_obj;
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   bool got_data = false;
  //   ObObj const_data;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
