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

#ifndef OCEANBASE_SQL_RESOLVER_DML_OB_FILE_SQL_UTILS_H_
#define OCEANBASE_SQL_RESOLVER_DML_OB_FILE_SQL_UTILS_H_

#include "lib/string/ob_string.h"
#include "lib/container/ob_iarray.h"
#include "lib/allocator/ob_allocator.h"
#include "sql/resolver/dml/ob_file_table_def.h"

namespace oceanbase
{
namespace sql
{

// One cell of a file as it was read from the file. The text points into the
// buffer that read_file() allocated from the statement allocator.
struct ObFileSqlCell
{
  ObFileSqlCell() : text_(), is_null_(false) {}
  common::ObString text_;
  bool is_null_;
};

// Options that can be passed to the read_xxx() table functions.
struct ObFileSqlOptions
{
  ObFileSqlOptions()
    : format_(FS_FMT_INVALID),
      has_header_(false),
      header_(false),
      has_delim_(false),
      delim_(','),
      has_quote_(false),
      quote_('"'),
      has_escape_(false),
      escape_('"'),
      has_nullstr_(false),
      nullstr_(),
      has_sample_size_(false),
      sample_size_(-1) {}
  // generic
  ObFileSqlFormat format_;
  // csv
  bool has_header_;
  bool header_;
  bool has_delim_;
  char delim_;
  bool has_quote_;
  char quote_;
  bool has_escape_;
  char escape_;
  bool has_nullstr_;
  common::ObString nullstr_;
  // number of rows used for type inference, only -1 (whole file) is supported
  bool has_sample_size_;
  int64_t sample_size_;
};

// Reads CSV / JSONL / Parquet files and turns them into a schema plus a matrix
// of cells. Every buffer used for the file content comes from the allocator
// that is passed to read_file(), so the result stays valid as long as the
// statement does.
class ObFileSqlReader
{
public:
  explicit ObFileSqlReader(common::ObIAllocator &alloc) : allocator_(alloc) {}
  ~ObFileSqlReader() {}

  // Read the whole file, infer the schema and fill in cell texts (row major:
  // cells.at(row * column_cnt + col)). The row count is stored in def.row_cnt_.
  int read_file(const common::ObString &path,
                const ObFileSqlFormat format,
                const ObFileSqlOptions &options,
                ObFileTableDef &def,
                common::ObIArray<ObFileSqlCell> &cells);

  // POSIX glob(), the result is sorted by base name.
  int glob_files(const common::ObString &pattern,
                 common::ObIArray<common::ObString> &files);

  static int detect_format_by_extension(const common::ObString &path,
                                       ObFileSqlFormat &format);

private:
  // Reads the file into a buffer owned by the statement allocator.
  int load_file_content(const common::ObString &path, char *&buf, int64_t &size);
  // Splits rows/cells, fills column names and the raw cell matrix.
  int parse_csv(char *buf, const int64_t size, const ObFileSqlOptions &options,
                ObFileTableDef &def, common::ObIArray<ObFileSqlCell> &cells);
  int parse_jsonl(char *buf, const int64_t size, ObFileTableDef &def,
                  common::ObIArray<ObFileSqlCell> &cells);
  int parse_parquet(const char *buf, const int64_t size, ObFileTableDef &def,
                    common::ObIArray<ObFileSqlCell> &cells);
  // Infer def.columns_[i].type_ / nullable_ from the cell matrix, and normalize
  // DATE / DATETIME cell texts.
  int infer_column_types(ObFileTableDef &def, common::ObIArray<ObFileSqlCell> &cells);
  // JSON numbers keep their own type, so the candidates differ from CSV.
  int infer_json_column_types(ObFileTableDef &def, common::ObIArray<ObFileSqlCell> &cells);
  // Decodes the common JSON escapes into out (out must have 4 bytes).
  static int json_unescape(const char esc, char *out, int64_t &out_len);

  int alloc_copy_string(const common::ObString &src, common::ObString &dst);
  int dedup_column_names(ObFileTableDef &def);

private:
  common::ObIAllocator &allocator_;
  DISALLOW_COPY_AND_ASSIGN(ObFileSqlReader);
};

// Text based type inference helpers, shared by the CSV and JSONL readers.
class ObFileSqlValueType
{
public:
  static bool is_bool_text(const char *ptr, const int64_t len);
  static bool is_int_text(const char *ptr, const int64_t len);
  static bool is_double_text(const char *ptr, const int64_t len);
  static bool is_date_text(const char *ptr, const int64_t len);
  static bool is_datetime_text(const char *ptr, const int64_t len);
  // 'T' separated timestamps are rewritten to ' ' in place.
  static void normalize_datetime_text(char *ptr, const int64_t len);
  static int64_t parse_int_text(const char *ptr, const int64_t len);
  static double parse_double_text(const char *ptr, const int64_t len);
};

}  // namespace sql
}  // namespace oceanbase

#endif  // OCEANBASE_SQL_RESOLVER_DML_OB_FILE_SQL_UTILS_H_
