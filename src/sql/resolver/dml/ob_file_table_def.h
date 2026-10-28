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

#ifndef OCEANBASE_SQL_RESOLVER_DML_OB_FILE_TABLE_DEF_H_
#define OCEANBASE_SQL_RESOLVER_DML_OB_FILE_TABLE_DEF_H_

#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace sql
{

// File formats understood by the read_xxx() table functions.
enum ObFileSqlFormat
{
  FS_FMT_INVALID = 0,
  FS_FMT_CSV = 1,
  FS_FMT_JSONL = 2,
  FS_FMT_PARQUET = 3,
};

// Logical (user visible) type of one column of a file table. It is only used
// to describe the file schema; the values themselves are carried by the
// values-table expressions.
enum ObFileSqlType
{
  FS_TYPE_BIGINT = 0,
  FS_TYPE_DOUBLE = 1,
  FS_TYPE_BOOLEAN = 2,
  FS_TYPE_VARCHAR = 3,
  FS_TYPE_DATE = 4,
  FS_TYPE_DATETIME = 5,
  FS_TYPE_NULL = 6,
  FS_TYPE_MAX,
};

const int64_t OB_FILE_MAX_COLUMN_CNT = 512;

struct ObFileColumnDef
{
  ObFileColumnDef() : type_(FS_TYPE_NULL), nullable_(true) {}
  common::ObString name_;
  ObFileSqlType type_;
  bool nullable_;
};

// Schema and metadata of a table produced by read_csv()/read_json()/
// read_parquet()/glob(). The rows themselves are expanded into a values table
// during resolution, so this struct only keeps the logical schema.
struct ObFileTableDef
{
  ObFileTableDef()
    : format_(FS_FMT_INVALID),
      column_cnt_(0),
      row_cnt_(0),
      is_glob_(false) {}
  ObFileSqlFormat format_;
  common::ObString path_;
  int64_t column_cnt_;
  int64_t row_cnt_;
  bool is_glob_;
  ObFileColumnDef columns_[OB_FILE_MAX_COLUMN_CNT];

  const char *get_type_name(const int64_t idx) const
  {
    static const char *type_names[FS_TYPE_MAX] = {
      "BIGINT", "DOUBLE", "BOOLEAN", "VARCHAR", "DATE", "DATETIME", "NULL"
    };
    const char *name = "VARCHAR";
    if (idx >= 0 && idx < column_cnt_) {
      const ObFileSqlType type = columns_[idx].type_;
      if (type >= FS_TYPE_BIGINT && type < FS_TYPE_MAX) {
        name = type_names[static_cast<int>(type)];
      }
    }
    return name;
  }

  int init(const ObFileSqlFormat format,
           const common::ObString &path,
           const int64_t column_cnt)
  {
    int ret = common::OB_SUCCESS;
    if (column_cnt <= 0 || column_cnt > OB_FILE_MAX_COLUMN_CNT) {
      ret = common::OB_INVALID_ARGUMENT;
    } else {
      format_ = format;
      path_ = path;
      column_cnt_ = column_cnt;
    }
    return ret;
  }
};

}  // namespace sql
}  // namespace oceanbase

#endif  // OCEANBASE_SQL_RESOLVER_DML_OB_FILE_TABLE_DEF_H_
