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

#define USING_LOG_PREFIX SHARE_SCHEMA
#include "ob_column_schema.h"
#include "share/schema/ob_schema_service.h"
#include "lib/string/ob_string_buffer.h"

namespace oceanbase
{
using namespace std;
using namespace common;
namespace share
{
namespace schema
{


ObColumnSchemaV2::ObColumnSchemaV2()
    : ObSchema(),
    local_session_vars_(get_allocator())
{
  reset();
}

ObColumnSchemaV2::ObColumnSchemaV2(ObIAllocator *allocator)
    : ObSchema(allocator),
    local_session_vars_(allocator)
{
  reset();
}

ObColumnSchemaV2::~ObColumnSchemaV2()
{
}

int ObColumnSchemaV2::assign(const ObColumnSchemaV2 &src_schema)
{
  int ret = OB_SUCCESS;
  if (this != &src_schema) {
    ObColumnSchemaV2::reset();
    error_ret_ = src_schema.error_ret_;
    table_id_ = src_schema.table_id_;
    column_id_ = src_schema.column_id_;
    schema_version_ = src_schema.schema_version_;
    rowkey_position_ = src_schema.rowkey_position_;
    index_position_ = src_schema.index_position_;
    order_in_rowkey_ = src_schema.order_in_rowkey_;
    tbl_part_key_pos_ = src_schema.tbl_part_key_pos_;
    meta_type_ = src_schema.meta_type_;
    accuracy_ = src_schema.accuracy_;
    is_nullable_ = src_schema.is_nullable_;
    is_zero_fill_ = src_schema.is_zero_fill_;
    is_autoincrement_ = src_schema.is_autoincrement_;
    is_hidden_ = src_schema.is_hidden_;
    charset_type_ = src_schema.charset_type_;
    is_binary_collation_ = src_schema.is_binary_collation_;
    on_update_current_timestamp_ = src_schema.on_update_current_timestamp_;
    column_flags_ = src_schema.column_flags_;
    prev_column_id_ = src_schema.prev_column_id_;
    next_column_id_ = src_schema.next_column_id_;
    encoding_type_ = src_schema.encoding_type_;
    srs_id_ = src_schema.srs_id_;
    udt_set_id_ = src_schema.udt_set_id_;
    sub_type_ = src_schema.sub_type_;
    skip_index_attr_ = src_schema.skip_index_attr_;
    lob_chunk_size_ = src_schema.lob_chunk_size_;

    if (OB_FAIL(deep_copy_obj(src_schema.orig_default_value_, orig_default_value_))) {
    } else if (OB_FAIL(deep_copy_obj(src_schema.cur_default_value_, cur_default_value_))) {
    } else if (OB_FAIL(deep_copy_str(src_schema.column_name_, column_name_))) {
    } else if (OB_FAIL(deep_copy_str(src_schema.comment_, comment_))) {
    } else if (OB_FAIL(set_extended_type_info(src_schema.extended_type_info_))) {
    } else if (src_schema.column_ref_idxs_ != NULL) {
      if (OB_FAIL(alloc_column_ref_set())) {
      } else if (OB_FAIL(column_ref_idxs_->add_members(*src_schema.column_ref_idxs_))) {
      }
    } else {/*do nothing*/}

    if (OB_SUCC(ret)) {
      if (OB_FAIL(local_session_vars_.deep_copy(src_schema.local_session_vars_))) {
      }
    }

    if (OB_FAIL(ret)) {
      error_ret_ = ret;
    }
  }
  return ret;
}

bool ObColumnSchemaV2::operator==(const ObColumnSchemaV2 &r) const
{
  return (table_id_ == r.table_id_ && column_id_ == r.column_id_
      && schema_version_ == r.schema_version_);
}

bool ObColumnSchemaV2::operator !=(const ObColumnSchemaV2 &r) const
{
  return !(*this == r);
}

int64_t ObColumnSchemaV2::get_convert_size() const
{
  int64_t convert_size = sizeof(*this);

  convert_size += orig_default_value_.get_deep_copy_size();
  convert_size += cur_default_value_.get_deep_copy_size();
  convert_size += column_name_.length() + 1;
  convert_size += comment_.length() + 1;
  if (column_ref_idxs_ != NULL) {
    convert_size += sizeof(ColumnReferenceSet);
  }
  convert_size += extended_type_info_.count() * static_cast<int64_t>(sizeof(ObString));
  for (int64_t i = 0; i < extended_type_info_.count(); ++i) {
    convert_size += extended_type_info_.at(i).length() + 1;
  }
  convert_size += local_session_vars_.get_deep_copy_size();
  return convert_size;
}

bool ObColumnSchemaV2::is_prefix_column() const
{
  bool bret = false;
  if (is_hidden() && is_generated_column()) {
    const char *prefix_str = "__substr";
    int64_t min_len = min(column_name_.length(), static_cast<int64_t>(strlen(prefix_str)));
    bret = (0 == strncasecmp(get_column_name(), prefix_str, min_len));
  }
  return bret;
}

bool ObColumnSchemaV2::is_func_idx_column() const
{
  bool bret = false;
  if (is_hidden() && is_generated_column()) {
    const char *func_idx_str = "SYS_NC";
    int64_t min_len = min(column_name_.length(), static_cast<int64_t>(strlen(func_idx_str)));
    bret = (0 == strncmp(get_column_name(), func_idx_str, min_len));
  }

  return bret;
}

void ObColumnSchemaV2::reset()
{
  table_id_ = OB_INVALID_ID;
  column_id_ = OB_INVALID_ID;
  schema_version_ = 0;
  rowkey_position_ = 0;
  index_position_ = 0;
  order_in_rowkey_ = ASC;
  tbl_part_key_pos_ = 0;
  meta_type_.reset();
  accuracy_.reset();
  is_nullable_ = true;
  is_zero_fill_ = false;
  is_autoincrement_ = false;
  is_hidden_ = false;
  charset_type_ = ObCharset::get_default_charset();
  is_binary_collation_ = false;
  on_update_current_timestamp_ = false;
  orig_default_value_.reset();
  cur_default_value_.reset();
  column_name_.reset();
  comment_.reset();
  column_flags_ = NON_CASCADE_FLAG;
  column_ref_idxs_ = NULL;
  prev_column_id_ = UINT64_MAX;
  next_column_id_ = UINT64_MAX;
  encoding_type_ = INT64_MAX;
  srs_id_ = OB_DEFAULT_COLUMN_SRS_ID;
  udt_set_id_ = 0;
  sub_type_ = 0;
  reset_string_array(extended_type_info_);
  skip_index_attr_.reset();
  lob_chunk_size_ = OB_DEFAULT_LOB_CHUNK_SIZE;
  local_session_vars_.reset();
  ObSchema::reset();
}

OB_DEF_SERIALIZE(ObColumnSchemaV2)
{
  int ret = OB_SUCCESS;
  bool has_column_ref = (column_ref_idxs_ != NULL);
  LST_DO_CODE(OB_UNIS_ENCODE,
              table_id_,
              column_id_,
              schema_version_,
              rowkey_position_,
              index_position_,
              order_in_rowkey_,
              tbl_part_key_pos_,
              meta_type_,
              accuracy_,
              is_nullable_,
              is_zero_fill_,
              is_autoincrement_,
              is_hidden_,
              charset_type_,
              is_binary_collation_,
              on_update_current_timestamp_,
              orig_default_value_,
              cur_default_value_,
              column_name_,
              comment_,
              column_flags_,
              has_column_ref);
  if (OB_SUCC(ret) && has_column_ref) {
    OB_UNIS_ENCODE(*column_ref_idxs_);
  }

  LST_DO_CODE(OB_UNIS_ENCODE,
              prev_column_id_,
              next_column_id_);

  if (!OB_SUCC(ret)) {
  } else if (OB_FAIL(serialize_string_array(buf, buf_len, pos, extended_type_info_))) {
  } else {
    LST_DO_CODE(OB_UNIS_ENCODE,
                srs_id_,
                udt_set_id_,
                sub_type_,
                skip_index_attr_,
                lob_chunk_size_,
                local_session_vars_);
  }

  return ret;
}

OB_DEF_DESERIALIZE(ObColumnSchemaV2)
{
  int ret = OB_SUCCESS;
  ObObj orig_default_value;
  ObObj cur_default_value;
  ObString column_name;
  ObString comment;
  bool has_column_ref = false;

  LST_DO_CODE(OB_UNIS_DECODE,
              table_id_,
              column_id_,
              schema_version_,
              rowkey_position_,
              index_position_,
              order_in_rowkey_,
              tbl_part_key_pos_,
              meta_type_,
              accuracy_,
              is_nullable_,
              is_zero_fill_,
              is_autoincrement_,
              is_hidden_,
              charset_type_,
              is_binary_collation_,
              on_update_current_timestamp_,
              orig_default_value,
              cur_default_value,
              column_name,
              comment,
              column_flags_,
              has_column_ref);
  if (OB_SUCC(ret) && has_column_ref) {
    if (OB_FAIL(alloc_column_ref_set())) {
    }
    OB_UNIS_DECODE(*column_ref_idxs_);
  }
  LST_DO_CODE(OB_UNIS_DECODE,
              prev_column_id_,
              next_column_id_);

  if (!OB_SUCC(ret)) {
  } else if (OB_FAIL(deserialize_string_array(buf, data_len, pos, extended_type_info_, get_allocator()))) {
  } else if (OB_FAIL(deep_copy_obj(orig_default_value, orig_default_value_))) {
  } else if (OB_FAIL(deep_copy_obj(cur_default_value, cur_default_value_))) {
  } else if (OB_FAIL(deep_copy_str(column_name, column_name_))) {
  } else if (OB_FAIL(deep_copy_str(comment, comment_))) {
  } else {
    LST_DO_CODE(OB_UNIS_DECODE,
                srs_id_,
                udt_set_id_,
                sub_type_,
                skip_index_attr_,
                lob_chunk_size_,
                local_session_vars_);
  }
  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObColumnSchemaV2)
{
  int64_t len = 0;
  bool has_column_ref = (column_ref_idxs_ != NULL);
  LST_DO_CODE(OB_UNIS_ADD_LEN,
              table_id_,
              column_id_,
              schema_version_,
              rowkey_position_,
              index_position_,
              order_in_rowkey_,
              tbl_part_key_pos_,
              meta_type_,
              accuracy_,
              is_nullable_,
              is_zero_fill_,
              is_autoincrement_,
              is_hidden_,
              charset_type_,
              is_binary_collation_,
              on_update_current_timestamp_,
              orig_default_value_,
              cur_default_value_,
              column_name_,
              comment_,
              column_flags_,
              has_column_ref);
  if (has_column_ref) {
    OB_UNIS_ADD_LEN(*column_ref_idxs_);
  }
  LST_DO_CODE(OB_UNIS_ADD_LEN,
              prev_column_id_,
              next_column_id_);
  len += get_string_array_serialize_size(extended_type_info_);
  LST_DO_CODE(OB_UNIS_ADD_LEN,
              srs_id_,
              udt_set_id_,
              sub_type_,
              skip_index_attr_,
              lob_chunk_size_,
              local_session_vars_);
  return len;
}

int ObColumnSchemaV2::set_part_key_pos(const int64_t part_key_pos)
{
  int ret = OB_SUCCESS;
  if (part_key_pos > UINT8_MAX) {
    ret =OB_ERR_UNEXPECTED;
  } else {
    part_pos_.part_key_pos_ = static_cast<uint8_t>(part_key_pos);
    if (0 != part_pos_.part_key_pos_) {
      add_column_flag(TABLE_PART_KEY_COLUMN_FLAG);
    }
  }
  return ret;
}

int ObColumnSchemaV2::set_subpart_key_pos(const int64_t subpart_key_pos)
{
  int ret = OB_SUCCESS;
  if (subpart_key_pos > UINT8_MAX) {
    ret =OB_ERR_UNEXPECTED;
  } else {
    part_pos_.subpart_key_pos_ = static_cast<uint8_t>(subpart_key_pos);
    if (0 != part_pos_.subpart_key_pos_) {
      add_column_flag(TABLE_PART_KEY_COLUMN_FLAG);
    }
  }
  return ret;
}



int64_t ObColumnSchemaV2::to_string(char *buf, const int64_t buf_len) const
{
  int64_t pos = 0;

  J_OBJ_START();
  J_KV(K_(table_id),
    K_(column_id),
    K_(schema_version),
    K_(rowkey_position),
    K_(index_position),
    K_(order_in_rowkey),
    K_(tbl_part_key_pos),
    K_(meta_type),
    K_(accuracy),
    K_(is_nullable),
    K_(is_zero_fill),
    K_(is_autoincrement),
    K_(is_hidden),
    K_(charset_type),
    K_(on_update_current_timestamp),
    K_(orig_default_value),
    K_(cur_default_value),
    K_(column_name),
    K_(comment),
    K_(column_flags),
    K_(extended_type_info),
    K_(prev_column_id),
    K_(next_column_id),
    K_(encoding_type),
    K_(srs_id),
    K_(udt_set_id),
    K_(sub_type),
    K_(skip_index_attr),
    K_(lob_chunk_size),
    KPC_(column_ref_idxs),
    K(local_session_vars_));
  J_OBJ_END();
  return pos;
}

int ObColumnSchemaV2::get_byte_length(
    int64_t &length,
    const bool for_check_length) const
{
  int ret = OB_SUCCESS;
  if (CS_TYPE_INVALID == get_collation_type()
      && !ob_is_extend(meta_type_.get_type())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ob_is_text_tc(meta_type_.get_type()) || ob_is_json(meta_type_.get_type())
             || ob_is_geometry(meta_type_.get_type())) {
    if (for_check_length) {
      // when check row length, a lob will occupy at most 512B
      length = min(get_data_length(), OB_MAX_LOB_HANDLE_LENGTH);
    } else {
      length = get_data_length();
    }
  } else {
    int64_t mbmaxlen = 0;
    if (OB_FAIL(ObCharset::get_mbmaxlen_by_coll(get_collation_type(), mbmaxlen))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      length = get_data_length() * mbmaxlen;
    }
  }
  return ret;
}

int ObColumnSchemaV2::alloc_column_ref_set()
{
  int ret = OB_SUCCESS;
  void *ptr = NULL;
  if (OB_ISNULL(ptr = alloc(sizeof(ColumnReferenceSet)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("allocate cascaded column set failed");
  } else {
    column_ref_idxs_ = new(ptr) ColumnReferenceSet();
  }
  return ret;
}

int ObColumnSchemaV2::add_cascaded_column_id(uint64_t column_id)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(column_ref_idxs_)) {
    if (OB_FAIL(alloc_column_ref_set())) {
    }
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(column_ref_idxs_->add_member(column_id - OB_APP_MIN_COLUMN_ID))) {
    } else {
      LOG_DEBUG("succ to add_cascaded_column_id", K(ret), K(*this), K(column_id), K(lbt()));
    }
  }
  return ret;
}

int ObColumnSchemaV2::del_cascaded_column_id(const uint64_t column_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(column_id < OB_APP_MIN_COLUMN_ID)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(column_ref_idxs_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(column_ref_idxs_->del_member(column_id - OB_APP_MIN_COLUMN_ID))) {
  } else {
    // do nothing
  }
  return ret;
}

bool ObColumnSchemaV2::has_cascaded_column_id(uint64_t column_id) const
{
  bool bret = false;
  if (column_ref_idxs_ != NULL) {
    bret = column_ref_idxs_->has_member(column_id - OB_APP_MIN_COLUMN_ID);
  }
  return bret;
}

int ObColumnSchemaV2::get_cascaded_column_ids(ObIArray<uint64_t> &column_ids) const
{
  int ret = OB_SUCCESS;
  if (column_ref_idxs_ != NULL) {
    for (int64_t i = 0; OB_SUCC(ret) && i < column_ref_idxs_->bit_count(); ++i) {
      if (column_ref_idxs_->has_member(i)) {
        if (OB_FAIL(column_ids.push_back(i + OB_APP_MIN_COLUMN_ID))) {
        }
      }
    }
  }
  return ret;
}

int ObColumnSchemaV2::set_extended_type_info(const ObIArray<common::ObString> &info)
{
  return deep_copy_string_array(info, extended_type_info_);
}

int ObColumnSchemaV2::add_type_info(const ObString &type_info)
{
  return add_string_to_array(type_info, extended_type_info_);
}

namespace
{
constexpr int64_t AI_COLUMN_META_FIELD_CNT = 11;
constexpr char AI_COLUMN_META_SEP = '\x1f';
constexpr ObSysVarClassType AI_COLUMN_META_VAR_TYPE = SYS_VAR_SQL_MODE;

void get_ai_meta_field(const ObString &meta, int64_t field_idx, ObString &field)
{
  field.reset();
  int64_t cur_field = 0;
  int64_t start = 0;
  while (cur_field <= field_idx && start <= meta.length()) {
    int64_t end = start;
    while (end < meta.length() && meta[end] != AI_COLUMN_META_SEP) {
      ++end;
    }
    if (cur_field == field_idx) {
      if (end > start) {
        field.assign_ptr(meta.ptr() + start, static_cast<int32_t>(end - start));
      }
      break;
    }
    if (end >= meta.length()) {
      break;
    }
    start = end + 1;
    ++cur_field;
  }
}
} // namespace

int ObColumnSchemaV2::get_ai_meta_str(ObString &meta) const
{
  int ret = OB_SUCCESS;
  ObSessionSysVar *var = NULL;
  meta.reset();
  if (is_ai_column() && OB_FAIL(local_session_vars_.get_local_var(AI_COLUMN_META_VAR_TYPE, var))) {
  } else if (OB_NOT_NULL(var)) {
    if (var->val_.is_varchar()) {
      meta = var->val_.get_string();
    } else {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

int ObColumnSchemaV2::set_ai_meta_field(int64_t field_idx, const ObString &value)
{
  int ret = OB_SUCCESS;
  ObString meta;
  ObString fields[AI_COLUMN_META_FIELD_CNT];
  ObIAllocator *alloc = get_allocator();
  if (field_idx < 0 || field_idx >= AI_COLUMN_META_FIELD_CNT) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(alloc)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column meta allocator is null", K(ret), K(field_idx));
  } else if (OB_FAIL(get_ai_meta_str(meta))) {
  } else {
    for (int64_t i = 0; i < AI_COLUMN_META_FIELD_CNT; ++i) {
      get_ai_meta_field(meta, i, fields[i]);
    }
    fields[field_idx] = value;
    ObStringBuffer buf(alloc);
    for (int64_t i = 0; OB_SUCC(ret) && i < AI_COLUMN_META_FIELD_CNT; ++i) {
      if (i > 0 && OB_FAIL(buf.append(&AI_COLUMN_META_SEP, 1))) {
      } else if (OB_FAIL(buf.append(fields[i].ptr(), fields[i].length()))) {
      }
    }
    ObString new_meta;
    if (OB_SUCC(ret) && OB_FAIL(buf.get_result_string(new_meta))) {
    } else if (OB_SUCC(ret)) {
      local_session_vars_.reset();
      local_session_vars_.set_allocator(alloc);
      if (OB_FAIL(local_session_vars_.set_local_var_capacity(1))) {
      }
      ObObj value_obj;
      value_obj.set_varchar(new_meta);
      value_obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      value_obj.set_collation_level(CS_LEVEL_IMPLICIT);
      if (OB_SUCC(ret)
          && OB_FAIL(local_session_vars_.add_local_var(AI_COLUMN_META_VAR_TYPE, value_obj))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("set ai column meta field failed", K(ret), K(field_idx), K(alloc));
  }
  return ret;
}

int ObColumnSchemaV2::set_ai_column_expr(const ObString &expr)
{
  return set_ai_meta_field(0, expr);
}

ObString ObColumnSchemaV2::get_ai_column_expr() const
{
  ObString meta;
  ObString field;
  if (OB_SUCCESS == get_ai_meta_str(meta)) {
    get_ai_meta_field(meta, 0, field);
  }
  return field;
}

int ObColumnSchemaV2::set_ai_column_max_retries(int64_t max_retries)
{
  int ret = OB_SUCCESS;
  char buf[24] = {0};
  int64_t pos = 0;
  if (OB_FAIL(databuff_printf(buf, sizeof(buf), pos, "%ld", max_retries))) {
  } else if (OB_FAIL(set_ai_meta_field(1, ObString(pos, buf)))) {
  }
  return ret;
}

int64_t ObColumnSchemaV2::get_ai_column_max_retries() const
{
  int64_t ret = 0;
  ObString meta;
  ObString field;
  if (OB_SUCCESS == get_ai_meta_str(meta)) {
    get_ai_meta_field(meta, 1, field);
    if (!field.empty()) {
      ret = strtoll(field.ptr(), nullptr, 10);
    }
  }
  return ret;
}

int ObColumnSchemaV2::set_ai_column_status_column_id(uint64_t column_id)
{
  int ret = OB_SUCCESS;
  char buf[24] = {0};
  int64_t pos = 0;
  if (OB_FAIL(databuff_printf(buf, sizeof(buf), pos, "%lu", column_id))) {
  } else if (OB_FAIL(set_ai_meta_field(2, ObString(pos, buf)))) {
  }
  return ret;
}

uint64_t ObColumnSchemaV2::get_ai_column_status_column_id() const
{
  uint64_t ret = OB_INVALID_ID;
  ObString meta;
  ObString field;
  if (OB_SUCCESS == get_ai_meta_str(meta)) {
    get_ai_meta_field(meta, 2, field);
    if (!field.empty()) {
      ret = strtoull(field.ptr(), nullptr, 10);
    }
  }
  return ret;
}

int ObColumnSchemaV2::set_ai_column_dep_names(const ObString &dep_names)
{
  return set_ai_meta_field(3, dep_names);
}

ObString ObColumnSchemaV2::get_ai_column_dep_names() const
{
  ObString meta;
  ObString field;
  if (OB_SUCCESS == get_ai_meta_str(meta)) {
    get_ai_meta_field(meta, 3, field);
  }
  return field;
}

int ObColumnSchemaV2::set_ai_column_gen_column_id(uint64_t column_id)
{
  int ret = OB_SUCCESS;
  char buf[24] = {0};
  int64_t pos = 0;
  if (OB_FAIL(databuff_printf(buf, sizeof(buf), pos, "%lu", column_id))) {
  } else if (OB_FAIL(set_ai_meta_field(4, ObString(pos, buf)))) {
  }
  return ret;
}

uint64_t ObColumnSchemaV2::get_ai_column_gen_column_id() const
{
  uint64_t ret = OB_INVALID_ID;
  ObString meta;
  ObString field;
  if (OB_SUCCESS == get_ai_meta_str(meta)) {
    get_ai_meta_field(meta, 4, field);
    if (!field.empty()) {
      ret = strtoull(field.ptr(), nullptr, 10);
    }
  }
  return ret;
}

int ObColumnSchemaV2::set_ai_column_embed_model(const ObString &model)
{
  return set_ai_meta_field(5, model);
}

ObString ObColumnSchemaV2::get_ai_column_embed_model() const
{
  ObString meta;
  ObString field;
  if (OB_SUCCESS == get_ai_meta_str(meta)) {
    get_ai_meta_field(meta, 5, field);
  }
  return field;
}

int ObColumnSchemaV2::set_ai_column_embed_input(const ObString &input)
{
  return set_ai_meta_field(6, input);
}

ObString ObColumnSchemaV2::get_ai_column_embed_input() const
{
  ObString meta;
  ObString field;
  if (OB_SUCCESS == get_ai_meta_str(meta)) {
    get_ai_meta_field(meta, 6, field);
  }
  return field;
}

int ObColumnSchemaV2::set_ai_column_embed_dimension(int64_t dimension)
{
  int ret = OB_SUCCESS;
  char buf[24] = {0};
  int64_t pos = 0;
  if (OB_FAIL(databuff_printf(buf, sizeof(buf), pos, "%ld", dimension))) {
  } else if (OB_FAIL(set_ai_meta_field(7, ObString(pos, buf)))) {
  }
  return ret;
}

int64_t ObColumnSchemaV2::get_ai_column_embed_dimension() const
{
  int64_t ret = 0;
  ObString meta;
  ObString field;
  if (OB_SUCCESS == get_ai_meta_str(meta)) {
    get_ai_meta_field(meta, 7, field);
    if (!field.empty()) {
      ret = strtoll(field.ptr(), nullptr, 10);
    }
  }
  return ret;
}

int ObColumnSchemaV2::set_ai_column_complete_model(const ObString &model)
{
  return set_ai_meta_field(8, model);
}

ObString ObColumnSchemaV2::get_ai_column_complete_model() const
{
  ObString meta;
  ObString field;
  if (OB_SUCCESS == get_ai_meta_str(meta)) {
    get_ai_meta_field(meta, 8, field);
  }
  return field;
}

int ObColumnSchemaV2::set_ai_column_complete_prompt(const ObString &prompt)
{
  return set_ai_meta_field(9, prompt);
}

ObString ObColumnSchemaV2::get_ai_column_complete_prompt() const
{
  ObString meta;
  ObString field;
  if (OB_SUCCESS == get_ai_meta_str(meta)) {
    get_ai_meta_field(meta, 9, field);
  }
  return field;
}

int ObColumnSchemaV2::set_ai_column_complete_config(const ObString &config)
{
  return set_ai_meta_field(10, config);
}

ObString ObColumnSchemaV2::get_ai_column_complete_config() const
{
  ObString meta;
  ObString field;
  if (OB_SUCCESS == get_ai_meta_str(meta)) {
    get_ai_meta_field(meta, 10, field);
  }
  return field;
}

int ObColumnSchemaV2::convert_column_id(const hash::ObHashMap<uint64_t, uint64_t> &column_id_map)
{
  int ret = OB_SUCCESS;
  ObSEArray<uint64_t, 1> old_column_ids;
  if (OB_FAIL(get_cascaded_column_ids(old_column_ids))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < old_column_ids.count(); i++) {
      const uint64_t old_column_id = old_column_ids.at(i);
      if (OB_FAIL(del_cascaded_column_id(old_column_id))) {
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < old_column_ids.count(); i++) {
      const uint64_t old_column_id = old_column_ids.at(i);
      uint64_t new_column_id = 0;
      if (OB_FAIL(column_id_map.get_refactored(old_column_id, new_column_id))) {
      } else if (OB_FAIL(add_cascaded_column_id(new_column_id))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else {
    const uint64_t old_column_id = get_column_id();
    uint64_t new_column_id = 0;
    if (OB_FAIL(column_id_map.get_refactored(old_column_id, new_column_id))) {
    } else {
      set_column_id(new_column_id);
    }
  }
  if (OB_FAIL(ret)) {
  } else {
    const uint64_t old_column_id = get_prev_column_id();
    uint64_t new_column_id = 0;
    if (old_column_id == BORDER_COLUMN_ID || old_column_id == UINT64_MAX) {
      // do nothing
    } else if (OB_FAIL(column_id_map.get_refactored(old_column_id, new_column_id))) {
    } else {
      set_prev_column_id(new_column_id);
    }
  }
  if (OB_FAIL(ret)) {
  } else {
    const uint64_t old_column_id = get_next_column_id();
    uint64_t new_column_id = 0;
    if (old_column_id == BORDER_COLUMN_ID || old_column_id == UINT64_MAX) {
      // do nothing
    } else if (OB_FAIL(column_id_map.get_refactored(old_column_id, new_column_id))) {
    } else {
      set_next_column_id(new_column_id);
    }
  }
  return ret;
}

int ObColumnSchemaV2::serialize_extended_type_info(char *buf, const int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(serialize_string_array(buf, buf_len, pos, extended_type_info_))) {
  }
  return ret;
}

int ObColumnSchemaV2::deserialize_extended_type_info(const char *buf,
                                                      const int64_t data_len,
                                                      int64_t &pos)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(deserialize_string_array(buf, data_len, pos, extended_type_info_, get_allocator()))) {
  }
  return ret;
}

int ObColumnSchemaV2::set_geo_type(const int32_t type_val)
{
  int ret = OB_SUCCESS;
  switch (type_val) {
    case static_cast<int32_t>(common::ObGeoType::GEOMETRY): {
      set_geo_type(common::ObGeoType::GEOMETRY);
      break;
    }

    case static_cast<int32_t>(common::ObGeoType::POINT): {
      set_geo_type(common::ObGeoType::POINT);
      break;
    }

    case static_cast<int32_t>(common::ObGeoType::LINESTRING): {
      set_geo_type(common::ObGeoType::LINESTRING);
      break;
    }

    case static_cast<int32_t>(common::ObGeoType::POLYGON): {
      set_geo_type(common::ObGeoType::POLYGON);
      break;
    }

    case static_cast<int32_t>(common::ObGeoType::MULTIPOINT): {
      set_geo_type(common::ObGeoType::MULTIPOINT);
      break;
    }

    case static_cast<int32_t>(common::ObGeoType::MULTILINESTRING): {
      set_geo_type(common::ObGeoType::MULTILINESTRING);
      break;
    }

    case static_cast<int32_t>(common::ObGeoType::MULTIPOLYGON): {
      set_geo_type(common::ObGeoType::MULTIPOLYGON);
      break;
    }

    case static_cast<int32_t>(common::ObGeoType::GEOMETRYCOLLECTION): {
      set_geo_type(common::ObGeoType::GEOMETRYCOLLECTION);
      break;
    }

    default: {
      ret = OB_ERR_UNEXPECTED;
      break;
    }
  }

  return ret;
}
int ObColumnSchemaV2::is_same_collection_column(const ObColumnSchemaV2 &other, bool &is_same) const
{
  int ret = OB_SUCCESS;
  if (get_extended_type_info().count() == other.get_extended_type_info().count()) {
    if (get_extended_type_info().count() != 1) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObString src_sub_name = get_extended_type_info().at(0);
      ObString dst_sub_name = other.get_extended_type_info().at(0);
      is_same = (src_sub_name.case_compare(dst_sub_name) == 0);
    }
  }
  return ret;
}

} //end of namespace schema
} //end of namespace share
} //end of namespace oceanbase
