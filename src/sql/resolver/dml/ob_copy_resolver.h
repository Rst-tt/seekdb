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

#ifndef SRC_SQL_RESOLVER_DML_OB_COPY_RESOLVER_H_
#define SRC_SQL_RESOLVER_DML_OB_COPY_RESOLVER_H_

#include "sql/resolver/dml/ob_dml_resolver.h"

namespace oceanbase
{
namespace sql
{

class ObSelectStmt;
class ObSelectIntoItem;

// COPY (query) TO 'file' (FORMAT CSV, HEADER, DELIMITER ',', ...)
// COPY table TO 'file' (...)
class ObCopyResolver : public ObDMLResolver
{
public:
  explicit ObCopyResolver(ObResolverParams &params)
      : ObDMLResolver(params),
        select_stmt_(NULL)
  {
  }
  virtual ~ObCopyResolver() {}
  virtual int resolve(const ParseNode &parse_tree) override;
private:
  int resolve_inner_select(const ParseNode &inner_node);
  int build_select_into(const ParseNode &path_node, const ParseNode *options_node);
  int resolve_copy_options(const ParseNode *options_node,
                           bool &has_header,
                           common::ObString &delimiter,
                           common::ObString &quote_str,
                           common::ObString &escape_str);
  int build_header_str(const ObSelectStmt &select_stmt, common::ObString &header);
  int get_option_name(const ParseNode &name_node, common::ObString &name);
private:
  ObSelectStmt *select_stmt_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObCopyResolver);
};

} // namespace sql
} // namespace oceanbase

#endif // SRC_SQL_RESOLVER_DML_OB_COPY_RESOLVER_H_
