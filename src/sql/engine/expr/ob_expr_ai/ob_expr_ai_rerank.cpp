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
#include "common/number/ob_number_v2.h"
#include "share/json/ob_json_helper.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{

namespace
{
const int64_t RERANK_BATCH_SIZE = 20;

bool is_number_node(const ObJsonNode *node)
{
  return OB_NOT_NULL(node) && share::ObJsonHelper::is_number_type(node);
}

int get_index_value(const ObJsonNode *node, int64_t &value)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    ret = OB_INVALID_DATA;
  } else if (node->json_type() == ObJsonNodeType::J_INT) {
    value = static_cast<const ObJsonInt *>(node)->value();
  } else if (node->json_type() == ObJsonNodeType::J_UINT) {
    value = static_cast<int64_t>(static_cast<const ObJsonUint *>(node)->value());
  } else {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

int get_score_value(ObIAllocator &allocator, const ObJsonNode *node, double &value)
{
  int ret = OB_SUCCESS;
  if (!is_number_node(node)) {
    ret = OB_INVALID_DATA;
  } else {
    share::ObJsonReaderHelper reader(allocator);
    float float_value = 0.0;
    if (OB_FAIL(reader.get_float_value(node, float_value))) {
    } else {
      value = float_value;
    }
  }
  return ret;
}

int score_to_double(const ObJsonNode *node, double &value)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    ret = OB_INVALID_DATA;
  } else {
    switch (node->json_type()) {
      case ObJsonNodeType::J_DOUBLE:
        value = static_cast<const ObJsonDouble *>(node)->value();
        break;
      case ObJsonNodeType::J_INT:
        value = static_cast<double>(static_cast<const ObJsonInt *>(node)->value());
        break;
      case ObJsonNodeType::J_UINT:
        value = static_cast<double>(static_cast<const ObJsonUint *>(node)->value());
        break;
      case ObJsonNodeType::J_DECIMAL: {
        number::ObNumber decimal_value = static_cast<const ObJsonDecimal *>(node)->value();
        char decimal_buf[512];
        int64_t pos = 0;
        if (OB_FAIL(decimal_value.format(decimal_buf, sizeof(decimal_buf), pos, -1))) {
        } else {
          decimal_buf[pos] = '\0';
          value = atof(decimal_buf);
        }
        break;
      }
      default:
        ret = OB_INVALID_DATA;
        break;
    }
  }
  return ret;
}

bool result_node_less(ObJsonObject *lhs, ObJsonObject *rhs)
{
  const ObString index_key("index");
  const ObString score_key("relevance_score");
  int64_t lhs_index = INT64_MAX;
  int64_t rhs_index = INT64_MAX;
  double lhs_score = -std::numeric_limits<double>::infinity();
  double rhs_score = -std::numeric_limits<double>::infinity();
  if (OB_SUCCESS != get_index_value(lhs->get_value(index_key), lhs_index)) {
    lhs_index = INT64_MAX;
  }
  if (OB_SUCCESS != get_index_value(rhs->get_value(index_key), rhs_index)) {
    rhs_index = INT64_MAX;
  }
  if (OB_SUCCESS != score_to_double(lhs->get_value(score_key), lhs_score)) {
    lhs_score = -std::numeric_limits<double>::infinity();
  }
  if (OB_SUCCESS != score_to_double(rhs->get_value(score_key), rhs_score)) {
    rhs_score = -std::numeric_limits<double>::infinity();
  }
  if (lhs_score != rhs_score) {
    return lhs_score > rhs_score;
  }
  return lhs_index < rhs_index;
}
} // namespace

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
                                    common::ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_type(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_type(types_stack[QUERY_IDX].get_type())) {
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

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
    res.set_null();
  } else if (arg_model_id->get_string().empty() || arg_query->get_string().empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query is empty");
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
    ObIJsonBase *documents_jbase = nullptr;
    ObJsonArray *document_array = nullptr;
    bool is_null_result = false;

    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
                                               documents_jbase, is_null_result))) {
    } else if (is_null_result || OB_ISNULL(documents_jbase)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, documents is null");
    } else if (documents_jbase->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (OB_ISNULL(document_array = static_cast<ObJsonArray *>(documents_jbase))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (document_array->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
    }

    bool has_doc_key = false;
    ObString doc_key;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_doc_key) && !arg_doc_key->is_null()) {
      doc_key = arg_doc_key->get_string();
      if (doc_key.empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key is empty");
      } else {
        has_doc_key = true;
      }
    }

    ObArray<ObString> doc_texts;
    if (OB_FAIL(ret)) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); i++) {
        ObJsonNode *doc_node = document_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_INVALID_ARGUMENT;
        } else if (has_doc_key) {
          if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
          } else {
            ObJsonNode *doc_value_node = static_cast<ObJsonObject *>(doc_node)->get_value(doc_key);
            if (OB_ISNULL(doc_value_node) || doc_value_node->json_type() != ObJsonNodeType::J_STRING) {
              ret = OB_INVALID_ARGUMENT;
              LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
            } else if (OB_FAIL(doc_texts.push_back(static_cast<ObJsonString *>(doc_value_node)->get_str()))) {
            }
          }
        } else {
          if (doc_node->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
          } else if (OB_FAIL(doc_texts.push_back(static_cast<ObJsonString *>(doc_node)->get_str()))) {
          }
        }
      }
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    }

    ObArray<ObString> header_array;
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(info)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_header(temp_allocator, *info, *endpoint_info, header_array))) {
    }

    ObArray<ObJsonObject *> all_result_nodes;
    if (OB_FAIL(ret)) {
    } else {
      for (int64_t start = 0; OB_SUCC(ret) && start < doc_texts.count(); start += RERANK_BATCH_SIZE) {
        int64_t end = start + RERANK_BATCH_SIZE;
        if (end > doc_texts.count()) {
          end = doc_texts.count();
        }
        ObJsonArray *batch_doc_array = nullptr;
        ObJsonArray *batch_result_array = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(temp_allocator, batch_doc_array))) {
        } else {
          for (int64_t i = start; OB_SUCC(ret) && i < end; i++) {
            ObJsonString *doc_str = nullptr;
            if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(temp_allocator, doc_texts.at(i), doc_str))) {
            } else if (OB_FAIL(batch_doc_array->append(doc_str))) {
            }
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(inner_eval_ai_rerank(temp_allocator, *info, *endpoint_info,
                                                header_array, query, batch_doc_array,
                                                batch_result_array))) {
        } else if (OB_ISNULL(batch_result_array) ||
                   batch_result_array->element_count() != static_cast<uint64_t>(end - start)) {
          ret = OB_INVALID_DATA;
        } else {
          ObArray<bool> seen;
          for (int64_t i = 0; i < end - start && OB_SUCC(ret); i++) {
            seen.push_back(false);
          }
          for (int64_t i = 0; OB_SUCC(ret) && i < end - start; i++) {
            ObJsonNode *item = batch_result_array->get_value(i);
            if (OB_ISNULL(item) || item->json_type() != ObJsonNodeType::J_OBJECT) {
              ret = OB_INVALID_DATA;
            } else {
              ObJsonObject *item_obj = static_cast<ObJsonObject *>(item);
              ObJsonNode *index_node = item_obj->get_value(INDEX_KEY);
              ObJsonNode *score_node = item_obj->get_value(SCORE_KEY);
              int64_t local_index = -1;
              double score = 0.0;
              if (OB_FAIL(get_index_value(index_node, local_index))) {
              } else if (OB_FAIL(get_score_value(temp_allocator, score_node, score))) {
              } else if (local_index < 0 || local_index >= end - start || seen.at(local_index)) {
                ret = OB_INVALID_DATA;
              } else {
                seen.at(local_index) = true;
                if (index_node->json_type() == ObJsonNodeType::J_INT) {
                  static_cast<ObJsonInt *>(index_node)->set_value(local_index + start);
                } else if (index_node->json_type() == ObJsonNodeType::J_UINT) {
                  static_cast<ObJsonUint *>(index_node)->set_value(static_cast<uint64_t>(local_index + start));
                }
                if (OB_FAIL(all_result_nodes.push_back(item_obj))) {
                }
              }
            }
          }
        }
      }
    }

    if (OB_FAIL(ret)) {
    } else if (all_result_nodes.count() != doc_texts.count()) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      std::sort(all_result_nodes.begin(), all_result_nodes.end(), result_node_less);
      ObJsonArray *output_array = nullptr;
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(temp_allocator, output_array))) {
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < all_result_nodes.count(); i++) {
          ObJsonObject *result_obj = all_result_nodes.at(i);
          if (OB_ISNULL(result_obj)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (!has_doc_key) {
            ObJsonNode *cloned = result_obj->clone(&temp_allocator, true);
            if (OB_ISNULL(cloned)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else if (OB_FAIL(output_array->append(cloned))) {
            }
          } else {
            int64_t original_index = -1;
            if (OB_FAIL(get_index_value(result_obj->get_value(INDEX_KEY), original_index))) {
            } else if (original_index < 0 || original_index >= document_array->element_count()) {
              ret = OB_INVALID_DATA;
            } else {
              ObJsonNode *origin_doc = document_array->get_value(original_index);
              ObJsonNode *score_node = result_obj->get_value(SCORE_KEY);
              ObJsonNode *cloned_doc = OB_ISNULL(origin_doc) ? nullptr
                                                             : origin_doc->clone(&temp_allocator, true);
              ObJsonNode *cloned_score = OB_ISNULL(score_node) ? nullptr
                                                               : score_node->clone(&temp_allocator, true);
              if (OB_ISNULL(cloned_doc) || OB_ISNULL(cloned_score) ||
                  cloned_doc->json_type() != ObJsonNodeType::J_OBJECT) {
                ret = OB_ALLOCATE_MEMORY_FAILED;
              } else if (OB_FAIL(static_cast<ObJsonObject *>(cloned_doc)->add(MODEL_SCORE_KEY, cloned_score))) {
              } else if (OB_FAIL(output_array->append(cloned_doc))) {
              }
            }
          }
        }
        if (OB_SUCC(ret)) {
          ObString raw_str;
          if (OB_FAIL(output_array->get_raw_binary(raw_str, &temp_allocator))) {
          } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank_with_doc_key(const ObExpr &expr, ObEvalCtx &ctx, ObIAllocator &allocator,
                                                ObString &model_id, ObString &query, ObJsonArray *document_array,
                                                ObString &doc_key, const ObAIFuncExprInfo &info,
                                                const share::ObAiModelEndpointInfo &endpoint_info, ObDatum &res)
{
  // Kept for compatibility. The main eval_ai_rerank path handles both string
  // arrays and object arrays with batching and stable ordering.
  UNUSED(expr);
  UNUSED(ctx);
  UNUSED(allocator);
  UNUSED(model_id);
  UNUSED(query);
  UNUSED(document_array);
  UNUSED(doc_key);
  UNUSED(info);
  UNUSED(endpoint_info);
  UNUSED(res);
  return OB_NOT_SUPPORTED;
}

int ObExprAIRerank::get_doc_array_from_documents_array_with_key(ObIAllocator &allocator, ObJsonArray *document_object_array, ObString &doc_key, ObJsonArray *&doc_array)
{
  INIT_SUCC(ret);
  ObJsonArray *doc_array_obj = nullptr;
  if (OB_ISNULL(document_object_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (doc_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, doc_array_obj))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < document_object_array->element_count(); i++) {
      ObJsonNode *doc_node = document_object_array->get_value(i);
      if (OB_ISNULL(doc_node) || doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        ObJsonNode *doc_value_node = static_cast<ObJsonObject *>(doc_node)->get_value(doc_key);
        if (OB_ISNULL(doc_value_node) || doc_value_node->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(doc_array_obj->append(doc_value_node))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    doc_array = doc_array_obj;
  }
  return ret;
}

int ObExprAIRerank::sort_document_array_by_model_result(ObIAllocator &allocator, ObJsonArray *document_array, ObJsonArray *model_result_array, ObJsonArray *&sorted_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *sorted_document_array_obj = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, sorted_document_array_obj))) {
  } else if (OB_ISNULL(document_array) || OB_ISNULL(model_result_array)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < model_result_array->element_count(); i++) {
      ObJsonNode *result_node = model_result_array->get_value(i);
      if (OB_ISNULL(result_node) || result_node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
      } else {
        ObJsonObject *result_obj = static_cast<ObJsonObject *>(result_node);
        int64_t index = -1;
        double score = 0.0;
        if (OB_FAIL(get_index_value(result_obj->get_value(INDEX_KEY), index))) {
        } else if (OB_FAIL(get_score_value(allocator, result_obj->get_value(SCORE_KEY), score))) {
        } else if (index < 0 || index >= document_array->element_count()) {
          ret = OB_INVALID_DATA;
        } else {
          ObJsonNode *origin_obj = document_array->get_value(index);
          ObJsonNode *new_obj = OB_ISNULL(origin_obj) ? nullptr : origin_obj->clone(&allocator, true);
          ObJsonNode *score_node = result_obj->get_value(SCORE_KEY);
          ObJsonNode *cloned_score = OB_ISNULL(score_node) ? nullptr : score_node->clone(&allocator, true);
          if (OB_ISNULL(new_obj) || OB_ISNULL(cloned_score) ||
              new_obj->json_type() != ObJsonNodeType::J_OBJECT) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(static_cast<ObJsonObject *>(new_obj)->add(MODEL_SCORE_KEY, cloned_score))) {
          } else if (OB_FAIL(sorted_document_array_obj->append(new_obj))) {
          }
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
  if (OB_ISNULL(document_array) || start_idx < 0 || end_idx < start_idx ||
      end_idx > document_array->element_count()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = start_idx; OB_SUCC(ret) && i < end_idx; i++) {
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
  } else if (OB_ISNULL(response) || response->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_DATA;
  } else if (OB_ISNULL(res = static_cast<ObJsonArray *>(response))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    result_array = res;
  }
  return ret;
}

int ObExprAIRerank::batch_result_add_base(ObIAllocator &allocator, ObJsonArray *array, int64_t start_idx)
{
  INIT_SUCC(ret);
  UNUSED(allocator);
  if (OB_ISNULL(array)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < array->element_count(); i++) {
      ObJsonNode *node = array->get_value(i);
      if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
      } else {
        ObJsonNode *index_node = static_cast<ObJsonObject *>(node)->get_value(INDEX_KEY);
        int64_t local_index = -1;
        if (OB_FAIL(get_index_value(index_node, local_index))) {
        } else if (index_node->json_type() == ObJsonNodeType::J_INT) {
          static_cast<ObJsonInt *>(index_node)->set_value(local_index + start_idx);
        } else {
          static_cast<ObJsonUint *>(index_node)->set_value(static_cast<uint64_t>(local_index + start_idx));
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::compact_json_array_by_key(ObIAllocator &allocator, ObJsonArray *array1, ObJsonArray *array2, ObString &score_key, ObJsonArray *&compact_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_ISNULL(array1) || OB_ISNULL(array2)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    int64_t i = 0;
    int64_t j = 0;
    while (OB_SUCC(ret) && i < array1->element_count() && j < array2->element_count()) {
      ObJsonNode *node1 = array1->get_value(i);
      ObJsonNode *node2 = array2->get_value(j);
      double score1 = 0.0;
      double score2 = 0.0;
      if (OB_ISNULL(node1) || OB_ISNULL(node2) ||
          node1->json_type() != ObJsonNodeType::J_OBJECT ||
          node2->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(get_score_value(allocator,
                                        static_cast<ObJsonObject *>(node1)->get_value(score_key),
                                        score1))) {
      } else if (OB_FAIL(get_score_value(allocator,
                                        static_cast<ObJsonObject *>(node2)->get_value(score_key),
                                        score2))) {
      } else if (score1 >= score2) {
        if (OB_FAIL(result_array->append(node1))) {
        }
        i++;
      } else if (OB_FAIL(result_array->append(node2))) {
        j++;
      }
    }
    while (OB_SUCC(ret) && i < array1->element_count()) {
      if (OB_FAIL(result_array->append(array1->get_value(i)))) {
      }
      i++;
    }
    while (OB_SUCC(ret) && j < array2->element_count()) {
      if (OB_FAIL(result_array->append(array2->get_value(j)))) {
      }
      j++;
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
  bool return_doc_value = return_doc > 0;
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
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
