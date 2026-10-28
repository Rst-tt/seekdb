/**
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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_copy_resolver.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "lib/charset/ob_charset.h"
#include "lib/string/ob_string.h"
#include "lib/utility/utility.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace oceanbase::common;
using namespace oceanbase::share;

namespace oceanbase
{
namespace sql
{

namespace copy_resolver_private
{
static const int64_t COPY_HEADER_SEPARATOR = '\x01';

static bool name_equal(const ObString &name, const char *literal)
{
  const int64_t literal_len = static_cast<int64_t>(strlen(literal));
  return name.length() == static_cast<int32_t>(literal_len)
         && 0 == strncasecmp(name.ptr(), literal, literal_len);
}

static int value_to_string(const ParseNode &node, char *buf, int64_t buf_len, ObString &out)
{
  int ret = OB_SUCCESS;
  if (T_IDENT == node.type_ || T_VARCHAR == node.type_ || T_CHAR == node.type_ || T_TEXT == node.type_) {
    out.assign_ptr(node.str_value_, node.str_len_);
  } else if (T_INT == node.type_ || T_BOOL == node.type_) {
    const int64_t len = snprintf(buf, static_cast<size_t>(buf_len), "%ld", node.value_);
    if (OB_UNLIKELY(len <= 0 || len >= buf_len)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to format integer value", K(ret), K(len));
    } else {
      out.assign_ptr(buf, static_cast<ObString::obstr_size_t>(len));
    }
  } else {
    ret = OB_ERR_PARSE_SQL;
    LOG_WARN("unexpected copy option value type", K(ret), "type", node.type_);
  }
  return ret;
}

static int value_to_bool(const ParseNode &node, bool &value)
{
  int ret = OB_SUCCESS;
  char buf[64];
  ObString text;
  if (T_BOOL == node.type_) {
    value = (0 != node.value_);
  } else if (OB_FAIL(value_to_string(node, buf, sizeof(buf), text))) {
  } else {
    value = text.empty() ? false : (name_equal(text, "true") || name_equal(text, "1")
                                    || name_equal(text, "on") || name_equal(text, "yes"));
  }
  return ret;
}
} // namespace copy_resolver_private

using namespace copy_resolver_private;

int ObCopyResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  const ParseNode *inner_node = NULL;
  if (OB_UNLIKELY(T_COPY != parse_tree.type_ || 3 != parse_tree.num_child_ || OB_ISNULL(parse_tree.children_))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid copy parse tree", K(ret), "type", parse_tree.type_, "num_child", parse_tree.num_child_);
  } else if (OB_ISNULL(inner_node = parse_tree.children_[0])
             || OB_ISNULL(parse_tree.children_[1])) {
    ret = OB_ERR_PARSE_SQL;
    LOG_WARN("invalid copy parse tree, inner query or path is null", K(ret));
  } else if (OB_FAIL(resolve_inner_select(*inner_node))) {
  } else if (OB_FAIL(build_select_into(*parse_tree.children_[1], parse_tree.children_[2]))) {
  }
  return ret;
}

int ObCopyResolver::resolve_inner_select(const ParseNode &inner_node)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(T_SELECT != inner_node.type_)) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("copy from table is not supported now", K(ret), "type", inner_node.type_);
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy from table");
  } else {
    ObSelectResolver select_resolver(params_);
    if (OB_FAIL(select_resolver.resolve(inner_node))) {
      LOG_WARN("failed to resolve inner select of copy", K(ret));
    } else if (OB_ISNULL(select_stmt_ = static_cast<ObSelectStmt*>(select_resolver.get_basic_stmt()))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("inner select stmt is null", K(ret));
    } else {
      set_basic_stmt(select_stmt_);
    }
  }
  return ret;
}

int ObCopyResolver::get_option_name(const ParseNode &name_node, ObString &name)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(T_IDENT != name_node.type_)) {
    ret = OB_ERR_PARSE_SQL;
    LOG_WARN("invalid copy option name", K(ret), "type", name_node.type_);
  } else if (OB_UNLIKELY(OB_ISNULL(name_node.str_value_))) {
    ret = OB_ERR_PARSE_SQL;
    LOG_WARN("invalid copy option name", K(ret), "type", name_node.type_);
  } else {
    name.assign_ptr(name_node.str_value_, name_node.str_len_);
  }
  return ret;
}

int ObCopyResolver::resolve_copy_options(const ParseNode *options_node,
                                         bool &has_header,
                                         ObString &delimiter,
                                         ObString &quote_str,
                                         ObString &escape_str)
{
  int ret = OB_SUCCESS;
  if (NULL == options_node) {
    // no options
  } else if (T_EXPR_LIST == options_node->type_ && 2 == options_node->num_child_
             && OB_NOT_NULL(options_node->children_)) {
    if (OB_FAIL(resolve_copy_options(options_node->children_[0], has_header, delimiter, quote_str, escape_str))) {
    } else if (OB_FAIL(resolve_copy_options(options_node->children_[1], has_header, delimiter, quote_str, escape_str))) {
    }
  } else {
    ObString name;
    const ParseNode *value_node = NULL;
    if (T_IDENT == options_node->type_) {
      if (OB_FAIL(get_option_name(*options_node, name))) {
      } else if (name_equal(name, "header")) {
        has_header = true;
      } else {
        ret = OB_ERR_PARSE_SQL;
        LOG_WARN("unsupported copy flag", K(ret), K(name));
      }
    } else {
      if (T_OP_EQ != options_node->type_ || 2 != options_node->num_child_ || OB_ISNULL(options_node->children_)) {
        ret = OB_ERR_PARSE_SQL;
        LOG_WARN("copy option needs a value", K(ret), "type", options_node->type_);
      } else if (OB_ISNULL(value_node = options_node->children_[1])) {
        ret = OB_ERR_PARSE_SQL;
      } else if (OB_FAIL(get_option_name(*options_node->children_[0], name))) {
      } else if (name_equal(name, "header")) {
        if (OB_FAIL(value_to_bool(*value_node, has_header))) {
        }
      } else if (name_equal(name, "format")) {
        char buf[64];
        ObString format_str;
        if (OB_FAIL(value_to_string(*value_node, buf, sizeof(buf), format_str))) {
        } else if (!name_equal(format_str, "csv")) {
          ret = OB_ERR_PARSE_SQL;
          LOG_WARN("copy only supports csv format", K(ret), K(format_str));
        }
      } else if (name_equal(name, "delimiter") || name_equal(name, "delim") || name_equal(name, "sep")) {
        char buf[64];
        ObString value;
        if (OB_FAIL(value_to_string(*value_node, buf, sizeof(buf), value))) {
        } else if (1 != value.length()) {
          ret = OB_ERR_PARSE_SQL;
          LOG_WARN("delimiter must be a single character", K(ret), K(value));
        } else {
          delimiter = value;
        }
      } else if (name_equal(name, "quote")) {
        char buf[64];
        ObString value;
        if (OB_FAIL(value_to_string(*value_node, buf, sizeof(buf), value))) {
        } else if (1 != value.length()) {
          ret = OB_ERR_PARSE_SQL;
          LOG_WARN("quote must be a single character", K(ret), K(value));
        } else {
          quote_str = value;
        }
      } else if (name_equal(name, "escape")) {
        char buf[64];
        ObString value;
        if (OB_FAIL(value_to_string(*value_node, buf, sizeof(buf), value))) {
        } else if (1 != value.length()) {
          ret = OB_ERR_PARSE_SQL;
          LOG_WARN("escape must be a single character", K(ret), K(value));
        } else {
          escape_str = value;
        }
      } else if (name_equal(name, "nullstr") || name_equal(name, "null")
                 || name_equal(name, "null_string")) {
        // accepted, null representation is handled by the csv writer
      } else {
        ret = OB_ERR_PARSE_SQL;
        LOG_WARN("unsupported copy option", K(ret), K(name));
      }
    }
  }
  return ret;
}

int ObCopyResolver::build_header_str(const ObSelectStmt &select_stmt, ObString &header)
{
  int ret = OB_SUCCESS;
  int64_t len = 0;
  char *buf = NULL;
  const int64_t select_cnt = select_stmt.get_select_item_size();
  if (OB_UNLIKELY(select_cnt <= 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("no select item to build copy header", K(ret), K(select_cnt));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < select_cnt; ++i) {
      const SelectItem &item = select_stmt.get_select_item(i);
      const ObString &name = (!item.alias_name_.empty()) ? item.alias_name_ : item.expr_name_;
      len += name.length() + (i > 0 ? 1 : 0);
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_UNLIKELY(len <= 0)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty copy header", K(ret), K(len));
    } else if (OB_ISNULL(buf = static_cast<char*>(allocator_->alloc(len)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate copy header buffer", K(ret), K(len));
    } else {
      int64_t pos = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < select_cnt; ++i) {
        const SelectItem &item = select_stmt.get_select_item(i);
        const ObString &name = (!item.alias_name_.empty()) ? item.alias_name_ : item.expr_name_;
        if (i > 0) {
          buf[pos++] = static_cast<char>(COPY_HEADER_SEPARATOR);
        }
        if (!name.empty()) {
          MEMCPY(buf + pos, name.ptr(), name.length());
          pos += name.length();
        }
      }
      header.assign_ptr(buf, static_cast<ObString::obstr_size_t>(pos));
    }
  }
  return ret;
}

int ObCopyResolver::build_select_into(const ParseNode &path_node, const ParseNode *options_node)
{
  int ret = OB_SUCCESS;
  bool file_sql_enabled = false;
  bool has_header = false;
  ObString delimiter;
  ObString quote_str;
  ObString escape_str;
  ObString path;
  ObString header;
  ObString secure_file_priv;
  ObString actual_path;
  char path_buf[PATH_MAX + 1];
  char real_path[PATH_MAX + 1];
  ObSelectIntoItem *into_item = NULL;
  if (OB_ISNULL(select_stmt_) || OB_ISNULL(allocator_) || OB_ISNULL(session_info_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("copy resolver is not inited", K(ret));
  } else if (OB_FAIL(session_info_->get_sys_variable(SYS_VAR_ENABLE_FILE_SQL, file_sql_enabled))) {
    LOG_WARN("failed to get enable_file_sql", K(ret));
  } else if (!file_sql_enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("file sql is disabled", K(ret));
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql is disabled, please set enable_file_sql to on first");
  } else if (OB_UNLIKELY(T_VARCHAR != path_node.type_ && T_CHAR != path_node.type_)
             || OB_ISNULL(path_node.str_value_) || path_node.str_len_ <= 0
             || path_node.str_len_ >= static_cast<int64_t>(sizeof(path_buf))) {
    ret = OB_ERR_PARSE_SQL;
    LOG_WARN("invalid copy target path", K(ret), "type", path_node.type_, "len", path_node.str_len_);
  } else if (FALSE_IT(MEMCPY(path_buf, path_node.str_value_, path_node.str_len_))) {
  } else if (FALSE_IT(path_buf[path_node.str_len_] = '\0')) {
  } else {
    char dir_buf[PATH_MAX + 1];
    const char *slash = strrchr(path_buf, '/');
    const char *base_name = NULL;
    char real_dir[PATH_MAX + 1];
    if (NULL == slash) {
      MEMCPY(dir_buf, ".", 2);
      base_name = path_buf;
    } else if (slash == path_buf) {
      MEMCPY(dir_buf, "/", 2);
      base_name = slash + 1;
    } else {
      const int64_t dir_len = slash - path_buf;
      MEMCPY(dir_buf, path_buf, dir_len);
      dir_buf[dir_len] = '\0';
      base_name = slash + 1;
    }
    if (OB_ISNULL(base_name) || '\0' == base_name[0]) {
      ret = OB_ERR_PARSE_SQL;
      LOG_WARN("invalid copy target file name", K(ret), K(path_buf));
    } else if (OB_ISNULL(realpath(dir_buf, real_dir))) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("copy target directory does not exist", K(ret), K(dir_buf), KERRMSG);
    } else if (0 == ::access(path_buf, F_OK)) {
      ret = OB_FILE_ALREADY_EXIST;
      LOG_WARN("copy target file already exists", K(ret), K(path_buf));
    } else {
      const int64_t len = snprintf(real_path, sizeof(real_path), "%s/%s", real_dir, base_name);
      if (OB_UNLIKELY(len <= 0 || len >= static_cast<int64_t>(sizeof(real_path)))) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("copy target path is too long", K(ret), K(len));
      } else {
        actual_path.assign_ptr(real_path, static_cast<ObString::obstr_size_t>(len));
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(session_info_->get_secure_file_priv(secure_file_priv))) {
    LOG_WARN("failed to get secure file priv", K(ret));
  } else if (!session_info_->is_inner()
             && OB_FAIL(ObResolverUtils::check_secure_path(secure_file_priv, actual_path))) {
    LOG_WARN("copy target is not in secure file priv", K(ret), K(secure_file_priv), K(actual_path));
  } else if (OB_FAIL(resolve_copy_options(options_node, has_header, delimiter, quote_str, escape_str))) {
  } else if (OB_FAIL(ob_write_string(*allocator_, actual_path, path))) {
    LOG_WARN("failed to copy copy target path", K(ret));
  } else {
    void *buf = NULL;
    if (OB_ISNULL(buf = allocator_->alloc(sizeof(ObSelectIntoItem)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate select into item", K(ret));
    } else {
      into_item = new (buf) ObSelectIntoItem();
      into_item->into_type_ = T_INTO_OUTFILE;
      into_item->cs_type_ = ObCharset::get_system_collation();
      ObObj path_obj;
      path_obj.set_varchar(path);
      path_obj.set_collation_type(into_item->cs_type_);
      path_obj.set_collation_level(CS_LEVEL_COERCIBLE);
      into_item->outfile_name_ = path_obj;
      into_item->is_single_ = true;
      ObString field_term = (delimiter.empty()) ? ObString(1, ",") : delimiter;
      ObString field_term_copy;
      if (OB_FAIL(ob_write_string(*allocator_, field_term, field_term_copy))) {
        LOG_WARN("failed to copy field terminator", K(ret));
      } else {
        ObObj field_obj;
        field_obj.set_varchar(field_term_copy);
        field_obj.set_collation_type(into_item->cs_type_);
        field_obj.set_collation_level(CS_LEVEL_COERCIBLE);
        into_item->field_str_ = field_obj;
      }
      into_item->has_header_ = has_header;
      if (OB_SUCC(ret) && has_header) {
        if (OB_FAIL(build_header_str(*select_stmt_, header))) {
        } else {
          into_item->header_str_ = header;
        }
      }
      if (OB_SUCC(ret)) {
        select_stmt_->set_select_into(into_item);
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
