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

#include "sql/resolver/dml/ob_file_sql_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include <algorithm>
#include <string>
#include <vector>

#include "lib/ob_errno.h"
#include "lib/utility/utility.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

namespace
{

// Files bigger than this are rejected: the resolver has to keep the rows in
// memory until the statement finishes.
const int64_t OB_FILE_SQL_MAX_CONTENT_SIZE = 512LL << 20;
const int64_t OB_FILE_SQL_MAX_ROW_CNT = 64LL << 20;

inline bool is_digit(const char c) { return c >= '0' && c <= '9'; }

int alloc_string(ObIAllocator &alloc,
                 const char *src,
                 const int64_t len,
                 ObString &dst)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  if (OB_UNLIKELY(len < 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (0 == len) {
    dst.reset();
  } else if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(len)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("allocate memory failed", K(ret), K(len));
  } else {
    memcpy(buf, src, len);
    dst.assign_ptr(buf, len);
  }
  return ret;
}

bool parse_int64_checked(const char *p, const int64_t len, int64_t &v)
{
  bool bret = true;
  int64_t i = 0;
  bool neg = false;
  uint64_t mag = 0;
  if (i < len && ('-' == p[i] || '+' == p[i])) {
    neg = ('-' == p[i]);
    ++i;
  }
  if (i >= len) {
    bret = false;
  }
  for (; bret && i < len; ++i) {
    if (!is_digit(p[i])) {
      bret = false;
    } else if (__builtin_mul_overflow(mag, 10ULL, &mag)
               || __builtin_add_overflow(mag, static_cast<uint64_t>(p[i] - '0'), &mag)) {
      bret = false;
    }
  }
  if (bret) {
    if (neg) {
      if (mag > static_cast<uint64_t>(INT64_MAX) + 1ULL) {
        bret = false;
      } else {
        v = static_cast<int64_t>(0 - mag);
      }
    } else {
      if (mag > static_cast<uint64_t>(INT64_MAX)) {
        bret = false;
      } else {
        v = static_cast<int64_t>(mag);
      }
    }
  }
  return bret;
}

// Syntax check of a decimal number, strtod() is too permissive (hex, inf, nan
// and leading white space).
bool number_syntax_ok(const char *p, const int64_t len)
{
  int64_t i = 0;
  int64_t digits = 0;
  bool bret = true;
  if (i < len && ('-' == p[i] || '+' == p[i])) {
    ++i;
  }
  for (; i < len && is_digit(p[i]); ++i) {
    ++digits;
  }
  if (i < len && '.' == p[i]) {
    ++i;
    for (; i < len && is_digit(p[i]); ++i) {
      ++digits;
    }
  }
  if (0 == digits) {
    bret = false;
  } else if (i < len && ('e' == p[i] || 'E' == p[i])) {
    ++i;
    if (i < len && ('-' == p[i] || '+' == p[i])) {
      ++i;
    }
    int64_t exp_digits = 0;
    for (; i < len && is_digit(p[i]); ++i) {
      ++exp_digits;
    }
    if (0 == exp_digits) {
      bret = false;
    }
  }
  return bret && i == len;
}

bool parse_double_checked(const char *p, const int64_t len, double &v)
{
  bool bret = false;
  char buf[64];
  char *endptr = NULL;
  if (len > 0 && len < static_cast<int64_t>(sizeof(buf)) && number_syntax_ok(p, len)) {
    memcpy(buf, p, len);
    buf[len] = '\0';
    errno = 0;
    v = strtod(buf, &endptr);
    if (endptr == buf + len && 0 == errno) {
      bret = true;
    }
  }
  return bret;
}

bool is_leap_year(const int64_t year)
{
  return (0 == year % 4 && 0 != year % 100) || 0 == year % 400;
}

int64_t days_of_month(const int64_t year, const int64_t month)
{
  static const int64_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int64_t cnt = 0;
  if (month >= 1 && month <= 12) {
    cnt = days[month - 1];
    if (2 == month && is_leap_year(year)) {
      cnt = 29;
    }
  }
  return cnt;
}

bool parse_ndigits(const char *p, const int64_t n, int64_t &v)
{
  bool bret = true;
  v = 0;
  for (int64_t i = 0; bret && i < n; ++i) {
    if (!is_digit(p[i])) {
      bret = false;
    } else {
      v = v * 10 + (p[i] - '0');
    }
  }
  return bret;
}

bool is_date_syntax(const char *p, const int64_t len)
{
  bool bret = false;
  int64_t year = 0;
  int64_t month = 0;
  int64_t day = 0;
  if (10 == len && '-' == p[4] && '-' == p[7]
      && parse_ndigits(p, 4, year)
      && parse_ndigits(p + 5, 2, month)
      && parse_ndigits(p + 8, 2, day)
      && year >= 1 && month >= 1 && month <= 12
      && day >= 1 && day <= days_of_month(year, month)) {
    bret = true;
  }
  return bret;
}

bool is_time_syntax(const char *p, const int64_t len)
{
  bool bret = false;
  int64_t hour = 0;
  int64_t minute = 0;
  int64_t second = 0;
  if (len >= 8 && ':' == p[2] && ':' == p[5]
      && parse_ndigits(p, 2, hour)
      && parse_ndigits(p + 3, 2, minute)
      && parse_ndigits(p + 6, 2, second)
      && hour <= 23 && minute <= 59 && second <= 59) {
    int64_t i = 8;
    bool frac_ok = true;
    if (i < len && '.' == p[i]) {
      ++i;
      int64_t frac_digits = 0;
      for (; i < len && is_digit(p[i]); ++i) {
        ++frac_digits;
      }
      if (0 == frac_digits || frac_digits > 9) {
        frac_ok = false;
      }
    }
    bret = frac_ok && i == len;
  }
  return bret;
}

bool is_datetime_syntax(const char *p, const int64_t len)
{
  bool bret = false;
  if (len >= 19 && is_date_syntax(p, 10) && (' ' == p[10] || 'T' == p[10])) {
    bret = is_time_syntax(p + 11, len - 11);
  }
  return bret;
}

struct CsvField
{
  CsvField() : ptr_(NULL), len_(0) {}
  char *ptr_;
  int64_t len_;
};

void trim_right(char *&ptr, int64_t &len)
{
  while (len > 0 && (' ' == ptr[len - 1] || '\t' == ptr[len - 1])) {
    --len;
  }
}

// Appends the text of a field to the cell matrix, empty texts are NULLs.
int add_cell(ObIAllocator &alloc,
             const char *ptr,
             const int64_t len,
             const ObFileSqlOptions &options,
             ObFileSqlCell &cell)
{
  int ret = OB_SUCCESS;
  bool is_null = (0 == len);
  if (!is_null && options.has_nullstr_ && options.nullstr_.length() == len
      && 0 == strncmp(ptr, options.nullstr_.ptr(), len)) {
    is_null = true;
  }
  cell.is_null_ = is_null;
  if (!is_null) {
    ret = alloc_string(alloc, ptr, len, cell.text_);
  }
  return ret;
}

int64_t last_dot_pos(const ObString &path)
{
  int64_t pos = -1;
  for (int64_t i = path.length() - 1; i >= 0; --i) {
    if ('.' == path[i]) {
      pos = i;
      break;
    }
  }
  return pos;
}

}  // namespace

bool ObFileSqlValueType::is_bool_text(const char *ptr, const int64_t len)
{
  bool bret = false;
  if (1 == len) {
    bret = ('0' == ptr[0] || '1' == ptr[0]);
  } else if (4 == len) {
    bret = (0 == strncasecmp(ptr, "true", 4));
  } else if (5 == len) {
    bret = (0 == strncasecmp(ptr, "false", 5));
  }
  return bret;
}

bool ObFileSqlValueType::is_int_text(const char *ptr, const int64_t len)
{
  int64_t v = 0;
  return parse_int64_checked(ptr, len, v);
}

bool ObFileSqlValueType::is_double_text(const char *ptr, const int64_t len)
{
  double v = 0.0;
  return parse_double_checked(ptr, len, v);
}

bool ObFileSqlValueType::is_date_text(const char *ptr, const int64_t len)
{
  return is_date_syntax(ptr, len);
}

bool ObFileSqlValueType::is_datetime_text(const char *ptr, const int64_t len)
{
  return is_datetime_syntax(ptr, len);
}

void ObFileSqlValueType::normalize_datetime_text(char *ptr, const int64_t len)
{
  if (len > 10 && 'T' == ptr[10]) {
    ptr[10] = ' ';
  }
}

int64_t ObFileSqlValueType::parse_int_text(const char *ptr, const int64_t len)
{
  int64_t v = 0;
  if (!parse_int64_checked(ptr, len, v)) {
    v = 0;
  }
  return v;
}

double ObFileSqlValueType::parse_double_text(const char *ptr, const int64_t len)
{
  double v = 0.0;
  if (!parse_double_checked(ptr, len, v)) {
    v = 0.0;
  }
  return v;
}

int ObFileSqlReader::json_unescape(const char esc, char *out, int64_t &out_len)
{
  int ret = OB_SUCCESS;
  out_len = 1;
  switch (esc) {
    case '"': out[0] = '"'; break;
    case '\\': out[0] = '\\'; break;
    case '/': out[0] = '/'; break;
    case 'b': out[0] = '\b'; break;
    case 'f': out[0] = '\f'; break;
    case 'n': out[0] = '\n'; break;
    case 'r': out[0] = '\r'; break;
    case 't': out[0] = '\t'; break;
    default:
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("unsupported json escape", K(ret), K(esc));
      break;
  }
  return ret;
}

int ObFileSqlReader::detect_format_by_extension(const ObString &path,
                                                ObFileSqlFormat &format)
{
  int ret = OB_SUCCESS;
  const char *ptr = path.ptr();
  const int64_t len = path.length();
  const int64_t dot = last_dot_pos(path);
  if (OB_UNLIKELY(len <= 0 || dot < 0 || dot + 1 >= len)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("can not detect the file format by the extension", K(ret), K(path));
  } else {
    const char *ext = ptr + dot + 1;
    const int64_t ext_len = len - dot - 1;
    if (7 == ext_len && 0 == strncasecmp(ext, "parquet", 7)) {
      format = FS_FMT_PARQUET;
    } else if ((4 == ext_len && 0 == strncasecmp(ext, "json", 4))
               || (5 == ext_len && 0 == strncasecmp(ext, "jsonl", 5))
               || (6 == ext_len && 0 == strncasecmp(ext, "ndjson", 6))) {
      format = FS_FMT_JSONL;
    } else if ((3 == ext_len && 0 == strncasecmp(ext, "csv", 3))
               || (3 == ext_len && 0 == strncasecmp(ext, "tsv", 3))
               || (3 == ext_len && 0 == strncasecmp(ext, "txt", 3))) {
      format = FS_FMT_CSV;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("unsupported file extension", K(ret), K(path));
    }
  }
  return ret;
}

int ObFileSqlReader::load_file_content(const ObString &path,
                                       char *&buf,
                                       int64_t &size)
{
  int ret = OB_SUCCESS;
  int fd = -1;
  buf = NULL;
  size = 0;
  char *file_name = NULL;
  const int64_t name_len = path.length();
  if (OB_ISNULL(file_name = static_cast<char *>(allocator_.alloc(name_len + 1)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("allocate memory failed", K(ret));
  } else {
    memcpy(file_name, path.ptr(), name_len);
    file_name[name_len] = '\0';
    if (-1 == (fd = ::open(file_name, O_RDONLY))) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("open file failed", K(ret), K(path), K(errno));
    }
  }
  if (OB_SUCC(ret)) {
    struct stat file_stat;
    if (0 != ::fstat(fd, &file_stat)) {
      ret = OB_IO_ERROR;
      LOG_WARN("fstat failed", K(ret), K(path), K(errno));
    } else if (!S_ISREG(file_stat.st_mode)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("only regular files are supported", K(ret), K(path));
    } else if (file_stat.st_size > OB_FILE_SQL_MAX_CONTENT_SIZE) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("the file is too big", K(ret), K(path), K(file_stat.st_size),
               K(OB_FILE_SQL_MAX_CONTENT_SIZE));
    } else if (0 == file_stat.st_size) {
      ret = OB_ITER_END;
      LOG_WARN("the file is empty", K(ret), K(path));
    } else if (OB_ISNULL(buf = static_cast<char *>(allocator_.alloc(file_stat.st_size + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("allocate memory failed", K(ret), K(file_stat.st_size));
    } else {
      int64_t readed = 0;
      while (readed < file_stat.st_size) {
        const ssize_t n = ::read(fd, buf + readed, file_stat.st_size - readed);
        if (n < 0) {
          if (EINTR == errno) {
            continue;
          }
          ret = OB_IO_ERROR;
          LOG_WARN("read file failed", K(ret), K(path), K(errno));
          break;
        } else if (0 == n) {
          break;
        } else {
          readed += n;
        }
      }
      if (OB_SUCC(ret)) {
        buf[readed] = '\0';
        size = readed;
      }
    }
  }
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
  return ret;
}

int ObFileSqlReader::parse_csv(char *buf,
                               const int64_t size,
                               const ObFileSqlOptions &options,
                               ObFileTableDef &def,
                               ObIArray<ObFileSqlCell> &cells)
{
  int ret = OB_SUCCESS;
  const char delim = options.has_delim_ ? options.delim_ : ',';
  const char quote = options.has_quote_ ? options.quote_ : '"';
  int64_t pos = 0;
  if (size >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF
      && static_cast<unsigned char>(buf[1]) == 0xBB
      && static_cast<unsigned char>(buf[2]) == 0xBF) {
    pos = 3;
  }
  std::vector<CsvField> fields;
  std::vector<int64_t> row_offsets;
  fields.reserve(64);
  row_offsets.reserve(64);
  while (pos < size) {
    if ('\n' == buf[pos]) {
      ++pos;
      continue;
    } else if ('\r' == buf[pos] && pos + 1 < size && '\n' == buf[pos + 1]) {
      pos += 2;
      continue;
    }
    row_offsets.push_back(static_cast<int64_t>(fields.size()));
    bool row_done = false;
    while (!row_done) {
      CsvField field;
      if (pos < size && buf[pos] == quote) {
        const int64_t out_start = pos + 1;
        int64_t out = pos + 1;
        ++pos;
        while (pos < size) {
          const char c = buf[pos];
          if (c == quote) {
            if (pos + 1 < size && buf[pos + 1] == quote) {
              buf[out++] = quote;
              pos += 2;
            } else {
              ++pos;
              break;
            }
          } else if ('\r' == c && pos + 1 < size && '\n' == buf[pos + 1]) {
            // normalize the CRLF inside a quoted field to LF
            ++pos;
          } else {
            buf[out++] = c;
            ++pos;
          }
        }
        // Trailing characters behind the closing quote are kept as they are.
        while (pos < size && buf[pos] != delim && '\n' != buf[pos] && '\r' != buf[pos]) {
          buf[out++] = buf[pos++];
        }
        field.ptr_ = buf + out_start;
        field.len_ = out - out_start;
      } else {
        const int64_t start = pos;
        while (pos < size && buf[pos] != delim && '\n' != buf[pos] && '\r' != buf[pos]) {
          ++pos;
        }
        field.ptr_ = buf + start;
        field.len_ = pos - start;
      }
      fields.push_back(field);
      if (pos < size && buf[pos] == delim) {
        ++pos;
        row_done = false;
      } else {
        row_done = true;
      }
    }
    if (pos < size && '\r' == buf[pos]) {
      ++pos;
    }
    if (pos < size && '\n' == buf[pos]) {
      ++pos;
    }
  }
  // The column count comes from the first row, every row has to match it.
  const int64_t row_cnt = static_cast<int64_t>(row_offsets.size());
  int64_t column_cnt = 0;
  if (OB_UNLIKELY(row_cnt <= 0)) {
    ret = OB_ITER_END;
    LOG_WARN("the csv file has no row", K(ret), K(def.path_));
  } else if (row_cnt > 1) {
    column_cnt = row_offsets.at(1) - row_offsets.at(0);
  } else {
    column_cnt = static_cast<int64_t>(fields.size());
  }
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; i < row_cnt; ++i) {
      const int64_t field_cnt = (i + 1 < row_cnt ? row_offsets.at(i + 1)
                                                : static_cast<int64_t>(fields.size()))
                                - row_offsets.at(i);
      if (field_cnt != column_cnt) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("the field count of the row does not match the header", K(ret),
                 K(i), K(field_cnt), K(column_cnt), K(def.path_));
        break;
      }
    }
  }
  if (OB_SUCC(ret) && (0 >= column_cnt || column_cnt > OB_FILE_MAX_COLUMN_CNT)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid column count", K(ret), K(column_cnt), K(def.path_));
  }
  if (OB_SUCC(ret)) {
    def.column_cnt_ = column_cnt;
    bool has_header = options.has_header_ ? options.header_ : false;
    if (!options.has_header_ && row_cnt > 1) {
      bool header_like = false;
      for (int64_t c = 0; !header_like && c < column_cnt; ++c) {
        const CsvField &first = fields.at(row_offsets.at(0) + c);
        const ObFileSqlType candidates[] = {
            FS_TYPE_BOOLEAN, FS_TYPE_BIGINT, FS_TYPE_DOUBLE, FS_TYPE_DATE, FS_TYPE_DATETIME
        };
        for (int64_t k = 0;
             k < static_cast<int64_t>(sizeof(candidates) / sizeof(candidates[0])) && !header_like;
             ++k) {
          const ObFileSqlType type = candidates[k];
          bool all_match = true;
          for (int64_t r = 1; all_match && r < row_cnt; ++r) {
            const CsvField &field = fields.at(row_offsets.at(r) + c);
            if (0 == field.len_) {
              continue;
            }
            switch (type) {
              case FS_TYPE_BOOLEAN:
                all_match = ObFileSqlValueType::is_bool_text(field.ptr_, field.len_);
                break;
              case FS_TYPE_BIGINT:
                all_match = ObFileSqlValueType::is_int_text(field.ptr_, field.len_);
                break;
              case FS_TYPE_DOUBLE:
                all_match = ObFileSqlValueType::is_double_text(field.ptr_, field.len_);
                break;
              case FS_TYPE_DATE:
                all_match = ObFileSqlValueType::is_date_text(field.ptr_, field.len_);
                break;
              default:
                all_match = ObFileSqlValueType::is_datetime_text(field.ptr_, field.len_);
                break;
            }
          }
          if (all_match) {
            bool first_match = false;
            switch (type) {
              case FS_TYPE_BOOLEAN:
                first_match = ObFileSqlValueType::is_bool_text(first.ptr_, first.len_);
                break;
              case FS_TYPE_BIGINT:
                first_match = ObFileSqlValueType::is_int_text(first.ptr_, first.len_);
                break;
              case FS_TYPE_DOUBLE:
                first_match = ObFileSqlValueType::is_double_text(first.ptr_, first.len_);
                break;
              case FS_TYPE_DATE:
                first_match = ObFileSqlValueType::is_date_text(first.ptr_, first.len_);
                break;
              default:
                first_match = ObFileSqlValueType::is_datetime_text(first.ptr_, first.len_);
                break;
            }
            header_like = !first_match;
          }
        }
      }
      has_header = header_like;
    }
    const int64_t first_data_row = has_header ? 1 : 0;
    const int64_t data_row_cnt = row_cnt - first_data_row;
    if (data_row_cnt > OB_FILE_SQL_MAX_ROW_CNT) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("too many rows", K(ret), K(data_row_cnt), K(OB_FILE_SQL_MAX_ROW_CNT));
    } else if (has_header) {
      for (int64_t c = 0; OB_SUCC(ret) && c < column_cnt; ++c) {
        CsvField &field = fields.at(row_offsets.at(0) + c);
        trim_right(field.ptr_, field.len_);
        if (OB_FAIL(alloc_string(allocator_, field.ptr_, field.len_, def.columns_[c].name_))) {
          LOG_WARN("copy column name failed", K(ret));
        }
      }
    } else {
      for (int64_t c = 0; OB_SUCC(ret) && c < column_cnt; ++c) {
        char name[32];
        const int64_t name_len = snprintf(name, sizeof(name), "column%ld", c);
        if (OB_FAIL(alloc_string(allocator_, name, name_len, def.columns_[c].name_))) {
          LOG_WARN("copy column name failed", K(ret));
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(dedup_column_names(def))) {
      LOG_WARN("dedup column names failed", K(ret));
    } else {
      for (int64_t r = 0; OB_SUCC(ret) && r < data_row_cnt; ++r) {
        for (int64_t c = 0; OB_SUCC(ret) && c < column_cnt; ++c) {
          const CsvField &field = fields.at(row_offsets.at(r + first_data_row) + c);
          ObFileSqlCell cell;
          if (OB_FAIL(add_cell(allocator_, field.ptr_, field.len_, options, cell))) {
            LOG_WARN("add cell failed", K(ret), K(r), K(c));
          } else if (OB_FAIL(cells.push_back(cell))) {
            LOG_WARN("push back cell failed", K(ret));
          }
        }
      }
      if (OB_SUCC(ret)) {
        def.row_cnt_ = data_row_cnt;
      }
    }
  }
  return ret;
}

int ObFileSqlReader::dedup_column_names(ObFileTableDef &def)
{
  int ret = OB_SUCCESS;
  std::vector<std::string> names;
  names.reserve(def.column_cnt_);
  for (int64_t i = 0; OB_SUCC(ret) && i < def.column_cnt_; ++i) {
    std::string base(def.columns_[i].name_.ptr(), def.columns_[i].name_.length());
    if (base.empty()) {
      char buf[32];
      snprintf(buf, sizeof(buf), "column%ld", i);
      base = buf;
    }
    std::string candidate = base;
    int64_t suffix = 1;
    while (std::find(names.begin(), names.end(), candidate) != names.end()) {
      char buf[32];
      ++suffix;
      snprintf(buf, sizeof(buf), "__%ld", suffix);
      candidate = base + buf;
    }
    names.push_back(candidate);
    if (candidate != base) {
      if (OB_FAIL(alloc_string(allocator_, candidate.data(), candidate.size(),
                               def.columns_[i].name_))) {
        LOG_WARN("copy column name failed", K(ret));
      }
    }
  }
  return ret;
}

int ObFileSqlReader::parse_jsonl(char *buf,
                                 const int64_t size,
                                 ObFileTableDef &def,
                                 ObIArray<ObFileSqlCell> &cells)
{
  int ret = OB_SUCCESS;
  std::vector<std::string> column_names;
  std::vector<std::vector<ObFileSqlCell> > rows;
  int64_t pos = 0;
  if (size >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF
      && static_cast<unsigned char>(buf[1]) == 0xBB
      && static_cast<unsigned char>(buf[2]) == 0xBF) {
    pos = 3;
  }
  int64_t row_no = 0;
  while (pos < size && OB_SUCC(ret)) {
    int64_t line_end = pos;
    while (line_end < size && '\n' != buf[line_end]) {
      ++line_end;
    }
    int64_t line_len = line_end - pos;
    if (line_len > 0 && '\r' == buf[pos + line_len - 1]) {
      --line_len;
    }
    int64_t begin = pos;
    while (begin < pos + line_len && (' ' == buf[begin] || '\t' == buf[begin])) {
      ++begin;
    }
    int64_t end = pos + line_len;
    while (end > begin && (' ' == buf[end - 1] || '\t' == buf[end - 1])) {
      --end;
    }
    pos = line_end + 1;
    if (begin == end) {
      continue;
    }
    std::vector<std::pair<std::string, std::string> > members;
    std::vector<bool> member_is_null;
    int64_t i = begin;
    if ('{' != buf[i]) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("the line of the jsonl file is not an object", K(ret), K(row_no), K(def.path_));
      break;
    }
    ++i;
    bool obj_done = false;
    while (!obj_done && OB_SUCC(ret)) {
      while (i < end && (' ' == buf[i] || '\t' == buf[i])) {
        ++i;
      }
      if (i < end && '}' == buf[i]) {
        ++i;
        obj_done = true;
        break;
      }
      if (i >= end || '"' != buf[i]) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("expect a key of the json object", K(ret), K(row_no), K(def.path_));
        break;
      }
      std::string key;
      ++i;
      bool key_done = false;
      while (i < end && !key_done) {
        if ('"' == buf[i]) {
          key_done = true;
          ++i;
        } else if ('\\' == buf[i] && i + 1 < end) {
          key.push_back(buf[i]);
          key.push_back(buf[i + 1]);
          i += 2;
        } else {
          key.push_back(buf[i]);
          ++i;
        }
      }
      if (!key_done) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("unterminated key", K(ret), K(row_no));
      }
      while (OB_SUCC(ret) && i < end && (' ' == buf[i] || '\t' == buf[i])) {
        ++i;
      }
      if (OB_FAIL(ret)) {
        break;
      } else if (i >= end || ':' != buf[i]) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("expect ':' after the key", K(ret), K(row_no));
        break;
      }
      ++i;
      while (i < end && (' ' == buf[i] || '\t' == buf[i])) {
        ++i;
      }
      std::string value;
      bool value_is_null = false;
      if (i >= end) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("missing value", K(ret), K(row_no));
      } else if ('"' == buf[i]) {
        ++i;
        bool value_done = false;
        while (i < end && !value_done && OB_SUCC(ret)) {
          if ('"' == buf[i]) {
            value_done = true;
            ++i;
          } else if ('\\' == buf[i] && i + 1 < end) {
            char out[4];
            int64_t out_len = 0;
            if (OB_FAIL(json_unescape(buf[i + 1], out, out_len))) {
              LOG_WARN("unescape json string failed", K(ret), K(row_no));
            } else {
              value.append(out, out_len);
              i += 2;
            }
          } else {
            value.push_back(buf[i]);
            ++i;
          }
        }
        if (!value_done && OB_SUCC(ret)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("unterminated string value", K(ret), K(row_no));
        }
      } else if ('{' == buf[i] || '[' == buf[i]) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("nested json value is not supported", K(ret), K(row_no), K(def.path_));
      } else {
        const int64_t value_begin = i;
        while (i < end && ',' != buf[i] && '}' != buf[i] && ' ' != buf[i]) {
          ++i;
        }
        if (i == value_begin) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("empty value", K(ret), K(row_no));
        } else {
          value.assign(buf + value_begin, i - value_begin);
          if ("null" == value) {
            value_is_null = true;
            value.clear();
          }
        }
      }
      if (OB_SUCC(ret)) {
        members.push_back(std::make_pair(key, value));
        member_is_null.push_back(value_is_null);
      }
      while (OB_SUCC(ret) && i < end && (' ' == buf[i] || '\t' == buf[i])) {
        ++i;
      }
      if (OB_FAIL(ret)) {
        break;
      } else if (i < end && ',' == buf[i]) {
        ++i;
      } else if (i < end && '}' == buf[i]) {
        ++i;
        obj_done = true;
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("expect ',' or '}' in the json object", K(ret), K(row_no));
      }
    }
    while (OB_SUCC(ret) && i < end && (' ' == buf[i] || '\t' == buf[i])) {
      ++i;
    }
    if (OB_SUCC(ret) && i != end) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("unexpected trailing characters", K(ret), K(row_no), K(def.path_));
    }
    if (OB_SUCC(ret)) {
      for (int64_t m = 0; m < static_cast<int64_t>(members.size()); ++m) {
        if (std::find(column_names.begin(), column_names.end(), members.at(m).first)
            == column_names.end()) {
          column_names.push_back(members.at(m).first);
        }
      }
      std::vector<ObFileSqlCell> row;
      for (int64_t m = 0; OB_SUCC(ret) && m < static_cast<int64_t>(members.size()); ++m) {
        const int64_t idx = std::find(column_names.begin(), column_names.end(),
                                      members.at(m).first) - column_names.begin();
        if (idx >= static_cast<int64_t>(row.size())) {
          const int64_t old_size = static_cast<int64_t>(row.size());
          row.resize(idx + 1);
          for (int64_t c = old_size; c <= idx; ++c) {
            row.at(c).is_null_ = true;
          }
        }
        ObFileSqlCell &cell = row.at(idx);
        if (member_is_null.at(m)) {
          cell.is_null_ = true;
          cell.text_.reset();
        } else {
          cell.is_null_ = false;
          if (OB_FAIL(alloc_string(allocator_, members.at(m).second.data(),
                                   members.at(m).second.size(), cell.text_))) {
            LOG_WARN("copy json value failed", K(ret));
          }
        }
      }
      if (OB_SUCC(ret)) {
        rows.push_back(row);
        ++row_no;
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (row_no > OB_FILE_SQL_MAX_ROW_CNT) {
      ret = OB_SIZE_OVERFLOW;
    } else if (row_no <= 0 || column_names.empty()) {
      ret = OB_ITER_END;
      LOG_WARN("the jsonl file has no row", K(ret), K(def.path_));
    } else if (static_cast<int64_t>(column_names.size()) > OB_FILE_MAX_COLUMN_CNT) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      def.column_cnt_ = static_cast<int64_t>(column_names.size());
      def.row_cnt_ = row_no;
      for (int64_t c = 0; OB_SUCC(ret) && c < def.column_cnt_; ++c) {
        if (OB_FAIL(alloc_string(allocator_, column_names.at(c).data(),
                                 column_names.at(c).size(), def.columns_[c].name_))) {
          LOG_WARN("copy column name failed", K(ret));
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(dedup_column_names(def))) {
        LOG_WARN("dedup column names failed", K(ret));
      } else {
        for (int64_t r = 0; OB_SUCC(ret) && r < row_no; ++r) {
          // The union of the fields is only known now, pad the short rows.
          const int64_t old_size = static_cast<int64_t>(rows.at(r).size());
          rows.at(r).resize(def.column_cnt_);
          for (int64_t c = old_size; c < def.column_cnt_; ++c) {
            rows.at(r).at(c).is_null_ = true;
          }
          for (int64_t c = 0; OB_SUCC(ret) && c < def.column_cnt_; ++c) {
            if (OB_FAIL(cells.push_back(rows.at(r).at(c)))) {
              LOG_WARN("push back cell failed", K(ret));
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObFileSqlReader::infer_column_types(ObFileTableDef &def,
                                        ObIArray<ObFileSqlCell> &cells)
{
  int ret = OB_SUCCESS;
  const int64_t column_cnt = def.column_cnt_;
  const int64_t row_cnt = def.row_cnt_;
  for (int64_t c = 0; OB_SUCC(ret) && c < column_cnt; ++c) {
    ObFileSqlType type = FS_TYPE_NULL;
    const ObFileSqlType candidates[] = {
        FS_TYPE_BOOLEAN, FS_TYPE_BIGINT, FS_TYPE_DOUBLE, FS_TYPE_DATE, FS_TYPE_DATETIME,
        FS_TYPE_VARCHAR
    };
    bool nullable = false;
    bool has_value = false;
    for (int64_t k = 0;
         k < static_cast<int64_t>(sizeof(candidates) / sizeof(candidates[0]))
         && FS_TYPE_NULL == type;
         ++k) {
      bool all_match = true;
      for (int64_t r = 0; all_match && r < row_cnt; ++r) {
        const ObFileSqlCell &cell = cells.at(r * column_cnt + c);
        if (cell.is_null_) {
          nullable = true;
          continue;
        }
        has_value = true;
        const char *ptr = cell.text_.ptr();
        const int64_t len = cell.text_.length();
        switch (candidates[k]) {
          case FS_TYPE_BOOLEAN:
            all_match = ObFileSqlValueType::is_bool_text(ptr, len);
            break;
          case FS_TYPE_BIGINT:
            all_match = ObFileSqlValueType::is_int_text(ptr, len);
            break;
          case FS_TYPE_DOUBLE:
            all_match = ObFileSqlValueType::is_double_text(ptr, len);
            break;
          case FS_TYPE_DATE:
            all_match = ObFileSqlValueType::is_date_text(ptr, len);
            break;
          case FS_TYPE_DATETIME:
            all_match = ObFileSqlValueType::is_datetime_text(ptr, len);
            break;
          default:
            all_match = true;
            break;
        }
      }
      if (all_match && has_value) {
        type = candidates[k];
      }
    }
    if (!has_value) {
      type = FS_TYPE_NULL;
    }
    def.columns_[c].type_ = type;
    def.columns_[c].nullable_ = nullable;
    if (FS_TYPE_DATETIME == type) {
      for (int64_t r = 0; r < row_cnt; ++r) {
        ObFileSqlCell &cell = cells.at(r * column_cnt + c);
        if (!cell.is_null_) {
          ObFileSqlValueType::normalize_datetime_text(cell.text_.ptr(), cell.text_.length());
        }
      }
    }
  }
  return ret;
}

int ObFileSqlReader::infer_json_column_types(ObFileTableDef &def,
                                             ObIArray<ObFileSqlCell> &cells)
{
  int ret = OB_SUCCESS;
  const int64_t column_cnt = def.column_cnt_;
  const int64_t row_cnt = def.row_cnt_;
  for (int64_t c = 0; OB_SUCC(ret) && c < column_cnt; ++c) {
    bool seen_value = false;
    bool all_bool = true;
    bool all_int = true;
    bool all_double = true;
    bool nullable = false;
    for (int64_t r = 0; r < row_cnt; ++r) {
      const ObFileSqlCell &cell = cells.at(r * column_cnt + c);
      if (cell.is_null_) {
        nullable = true;
        continue;
      }
      seen_value = true;
      const char *ptr = cell.text_.ptr();
      const int64_t len = cell.text_.length();
      const bool is_bool = (4 == len && 0 == strncasecmp(ptr, "true", 4))
                           || (5 == len && 0 == strncasecmp(ptr, "false", 5));
      if (!is_bool) {
        all_bool = false;
      }
      if (!ObFileSqlValueType::is_int_text(ptr, len)) {
        all_int = false;
      }
      if (!ObFileSqlValueType::is_double_text(ptr, len)) {
        all_double = false;
      }
    }
    ObFileSqlType type = FS_TYPE_NULL;
    if (seen_value) {
      if (all_bool) {
        type = FS_TYPE_BOOLEAN;
      } else if (all_int) {
        type = FS_TYPE_BIGINT;
      } else if (all_double) {
        type = FS_TYPE_DOUBLE;
      } else {
        type = FS_TYPE_VARCHAR;
      }
    }
    def.columns_[c].type_ = type;
    def.columns_[c].nullable_ = nullable;
  }
  return ret;
}

int ObFileSqlReader::read_file(const ObString &path,
                               const ObFileSqlFormat format,
                               const ObFileSqlOptions &options,
                               ObFileTableDef &def,
                               ObIArray<ObFileSqlCell> &cells)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  int64_t size = 0;
  def.format_ = format;
  def.path_ = path;
  def.column_cnt_ = 0;
  def.row_cnt_ = 0;
  if (OB_FAIL(load_file_content(path, buf, size))) {
    LOG_WARN("load file content failed", K(ret), K(path));
  } else if (FS_FMT_CSV == format || FS_FMT_INVALID == format) {
    if (OB_FAIL(parse_csv(buf, size, options, def, cells))) {
      LOG_WARN("parse csv file failed", K(ret), K(path));
    } else if (OB_FAIL(infer_column_types(def, cells))) {
      LOG_WARN("infer column types failed", K(ret));
    }
  } else if (FS_FMT_JSONL == format) {
    if (OB_FAIL(parse_jsonl(buf, size, def, cells))) {
      LOG_WARN("parse jsonl file failed", K(ret), K(path));
    } else if (OB_FAIL(infer_json_column_types(def, cells))) {
      LOG_WARN("infer column types failed", K(ret));
    }
  } else if (FS_FMT_PARQUET == format) {
    if (OB_FAIL(parse_parquet(buf, size, def, cells))) {
      LOG_WARN("parse parquet file failed", K(ret), K(path));
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid file format", K(ret), K(path), K(format));
  }
  return ret;
}

int ObFileSqlReader::glob_files(const ObString &pattern,
                                ObIArray<ObString> &files)
{
  int ret = OB_SUCCESS;
  char *pattern_str = NULL;
  const int64_t len = pattern.length();
  if (OB_UNLIKELY(len <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid glob pattern", K(ret), K(pattern));
  } else if (OB_ISNULL(pattern_str = static_cast<char *>(allocator_.alloc(len + 1)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    memcpy(pattern_str, pattern.ptr(), len);
    pattern_str[len] = '\0';
    glob_t glob_result;
    memset(&glob_result, 0, sizeof(glob_result));
    const int gret = ::glob(pattern_str, GLOB_NOSORT, NULL, &glob_result);
    if (GLOB_NOMATCH == gret) {
      ret = OB_ITER_END;
      LOG_WARN("no file matches the pattern", K(ret), K(pattern));
    } else if (0 != gret) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("glob failed", K(ret), K(pattern), K(gret));
    } else {
      std::vector<std::string> paths;
      paths.reserve(glob_result.gl_pathc);
      for (size_t i = 0; i < glob_result.gl_pathc; ++i) {
        struct stat file_stat;
        if (0 == ::stat(glob_result.gl_pathv[i], &file_stat) && S_ISREG(file_stat.st_mode)) {
          paths.push_back(std::string(glob_result.gl_pathv[i]));
        }
      }
      std::sort(paths.begin(), paths.end());
      for (size_t i = 0; OB_SUCC(ret) && i < paths.size(); ++i) {
        ObString name;
        if (OB_FAIL(alloc_string(allocator_, paths.at(i).data(), static_cast<int64_t>(paths.at(i).size()), name))) {
          LOG_WARN("copy path failed", K(ret));
        } else if (OB_FAIL(files.push_back(name))) {
          LOG_WARN("push back path failed", K(ret));
        }
      }
      if (OB_SUCC(ret) && paths.empty()) {
        ret = OB_ITER_END;
      }
    }
    ::globfree(&glob_result);
  }
  return ret;
}

// ---------------------------------------------------------------------------
// Parquet
// ---------------------------------------------------------------------------

namespace
{

// Thrift compact protocol, only the parts of the parquet metadata that are
// needed here.
class PqReader
{
public:
  PqReader(const char *data, const int64_t size)
    : data_(data), size_(size), pos_(0), last_id_(0) {}
  int64_t pos() const { return pos_; }
  void set_pos(const int64_t pos) { pos_ = pos; }
  bool read_byte(uint8_t &v)
  {
    bool bret = pos_ < size_;
    if (bret) {
      v = static_cast<uint8_t>(data_[pos_]);
      ++pos_;
    }
    return bret;
  }
  bool read_varint(uint64_t &v)
  {
    bool bret = true;
    int shift = 0;
    v = 0;
    while (bret) {
      uint8_t b = 0;
      if (!read_byte(b)) {
        bret = false;
      } else {
        v |= (static_cast<uint64_t>(b & 0x7f) << shift);
        shift += 7;
        if (0 == (b & 0x80)) {
          break;
        } else if (shift > 63) {
          bret = false;
        }
      }
    }
    return bret;
  }
  bool read_i64(int64_t &v)
  {
    uint64_t u = 0;
    bool bret = read_varint(u);
    if (bret) {
      v = static_cast<int64_t>((u >> 1) ^ static_cast<uint64_t>(-static_cast<int64_t>(u & 1)));
    }
    return bret;
  }
  bool read_i32(int32_t &v)
  {
    int64_t v64 = 0;
    bool bret = read_i64(v64);
    if (bret) {
      v = static_cast<int32_t>(v64);
    }
    return bret;
  }
  bool read_binary(const char *&ptr, int64_t &len)
  {
    uint64_t l = 0;
    bool bret = read_varint(l);
    if (bret) {
      if (pos_ + static_cast<int64_t>(l) > size_) {
        bret = false;
      } else {
        ptr = data_ + pos_;
        len = static_cast<int64_t>(l);
        pos_ += len;
      }
    }
    return bret;
  }
  bool read_string(std::string &s)
  {
    const char *p = NULL;
    int64_t l = 0;
    bool bret = read_binary(p, l);
    if (bret) {
      s.assign(p, l);
    }
    return bret;
  }
  bool read_list(uint8_t &elem_type, int64_t &count)
  {
    uint8_t b = 0;
    bool bret = read_byte(b);
    if (bret) {
      elem_type = b & 0x0f;
      const uint32_t sz = (b & 0xf0) >> 4;
      if (15 == sz) {
        uint64_t v = 0;
        bret = read_varint(v);
        count = static_cast<int64_t>(v);
      } else {
        count = static_cast<int64_t>(sz);
      }
    }
    return bret;
  }
  bool read_map_header(uint8_t &key_type, uint8_t &value_type, int64_t &count)
  {
    uint8_t b = 0;
    bool bret = read_byte(b);
    if (bret) {
      key_type = b & 0x0f;
      count = static_cast<int64_t>((b & 0xf0) >> 4);
      if (0 == count) {
        value_type = 0;
      } else {
        bret = read_byte(value_type);
      }
    }
    return bret;
  }
  bool next_field(uint8_t &type, int16_t &id)
  {
    uint8_t b = 0;
    bool bret = read_byte(b);
    if (bret) {
      if (0 == b) {
        type = 0;
      } else {
        type = b & 0x0f;
        const uint32_t delta = (b & 0xf0) >> 4;
        if (0 != delta) {
          id = static_cast<int16_t>(last_id_ + static_cast<int16_t>(delta));
        } else {
          int32_t z = 0;
          if (!read_i32(z)) {
            bret = false;
          }
          id = static_cast<int16_t>(z);
        }
        last_id_ = id;
      }
    }
    return bret;
  }
  bool skip(const uint8_t type)
  {
    bool bret = true;
    switch (type) {
      case 1:
      case 2:
        break;
      case 3: {
        uint8_t b = 0;
        bret = read_byte(b);
        break;
      }
      case 4:
      case 5:
      case 6: {
        uint64_t u = 0;
        bret = read_varint(u);
        break;
      }
      case 7: {
        if (pos_ + 8 > size_) {
          bret = false;
        } else {
          pos_ += 8;
        }
        break;
      }
      case 8: {
        const char *p = NULL;
        int64_t l = 0;
        bret = read_binary(p, l);
        break;
      }
      case 9:
      case 10: {
        uint8_t elem_type = 0;
        int64_t count = 0;
        bret = read_list(elem_type, count);
        for (int64_t i = 0; bret && i < count; ++i) {
          bret = skip(elem_type);
        }
        break;
      }
      case 11: {
        uint8_t key_type = 0;
        uint8_t value_type = 0;
        int64_t count = 0;
        bret = read_map_header(key_type, value_type, count);
        for (int64_t i = 0; bret && i < count; ++i) {
          bret = skip(key_type) && skip(value_type);
        }
        break;
      }
      case 12: {
        begin_struct();
        while (bret) {
          uint8_t t = 0;
          int16_t id = 0;
          if (!next_field(t, id)) {
            bret = false;
          } else if (0 == t) {
            break;
          } else if (!skip(t)) {
            bret = false;
          }
        }
        end_struct();
        break;
      }
      default:
        bret = false;
        break;
    }
    return bret;
  }
  void begin_struct()
  {
    stack_.push_back(last_id_);
    last_id_ = 0;
  }
  void end_struct()
  {
    if (!stack_.empty()) {
      last_id_ = stack_.back();
      stack_.pop_back();
    }
  }

private:
  const char *data_;
  int64_t size_;
  int64_t pos_;
  int16_t last_id_;
  std::vector<int16_t> stack_;
};

struct PqSchemaElem
{
  PqSchemaElem() : type_(-1), repetition_(-1), num_children_(0), converted_type_(-1) {}
  int32_t type_;
  int32_t repetition_;
  int32_t num_children_;
  int32_t converted_type_;
  std::string name_;
};

struct PqColMeta
{
  PqColMeta()
    : type_(-1), codec_(0), num_values_(0), total_compressed_size_(0),
      data_page_offset_(-1), dictionary_page_offset_(-1) {}
  int32_t type_;
  int32_t codec_;
  int64_t num_values_;
  int64_t total_compressed_size_;
  int64_t data_page_offset_;
  int64_t dictionary_page_offset_;
};

struct PqRowGroupMeta
{
  PqRowGroupMeta() : num_rows_(0) {}
  int64_t num_rows_;
  std::vector<PqColMeta> columns_;
};

struct PqFileMeta
{
  PqFileMeta() : num_rows_(0) {}
  int64_t num_rows_;
  std::vector<PqSchemaElem> schema_;
  std::vector<PqRowGroupMeta> row_groups_;
};

struct PqPageHeader
{
  PqPageHeader()
    : type_(-1), uncompressed_size_(0), compressed_size_(0), num_values_(0),
      encoding_(0), def_levels_byte_length_(0), is_v2_(false),
      is_compressed_v2_(true), num_dict_values_(0) {}
  int32_t type_;
  int32_t uncompressed_size_;
  int32_t compressed_size_;
  int32_t num_values_;
  int32_t encoding_;
  int32_t def_levels_byte_length_;
  bool is_v2_;
  bool is_compressed_v2_;
  int32_t num_dict_values_;
};

bool pq_parse_schema_element(PqReader &r, PqSchemaElem &elem)
{
  bool bret = true;
  r.begin_struct();
  while (bret) {
    uint8_t t = 0;
    int16_t id = 0;
    if (!r.next_field(t, id)) {
      bret = false;
    } else if (0 == t) {
      break;
    } else if (1 == id) {
      bret = r.read_i32(elem.type_);
    } else if (3 == id) {
      bret = r.read_i32(elem.repetition_);
    } else if (4 == id && 8 == t) {
      bret = r.read_string(elem.name_);
    } else if (5 == id) {
      bret = r.read_i32(elem.num_children_);
    } else if (6 == id) {
      bret = r.read_i32(elem.converted_type_);
    } else {
      bret = r.skip(t);
    }
  }
  r.end_struct();
  return bret;
}

bool pq_parse_col_meta(PqReader &r, PqColMeta &meta)
{
  bool bret = true;
  r.begin_struct();
  while (bret) {
    uint8_t t = 0;
    int16_t id = 0;
    if (!r.next_field(t, id)) {
      bret = false;
    } else if (0 == t) {
      break;
    } else if (1 == id) {
      bret = r.read_i32(meta.type_);
    } else if (2 == id) {
      uint8_t elem_type = 0;
      int64_t count = 0;
      bret = r.read_list(elem_type, count);
      for (int64_t i = 0; bret && i < count; ++i) {
        int32_t v = 0;
        bret = r.read_i32(v);
      }
    } else if (3 == id) {
      uint8_t elem_type = 0;
      int64_t count = 0;
      bret = r.read_list(elem_type, count);
      for (int64_t i = 0; bret && i < count; ++i) {
        std::string s;
        bret = r.read_string(s);
      }
    } else if (4 == id) {
      bret = r.read_i32(meta.codec_);
    } else if (5 == id) {
      bret = r.read_i64(meta.num_values_);
    } else if (7 == id) {
      bret = r.read_i64(meta.total_compressed_size_);
    } else if (9 == id) {
      bret = r.read_i64(meta.data_page_offset_);
    } else if (11 == id) {
      bret = r.read_i64(meta.dictionary_page_offset_);
    } else {
      bret = r.skip(t);
    }
  }
  r.end_struct();
  return bret;
}

bool pq_parse_column_chunk(PqReader &r, PqColMeta &meta)
{
  bool bret = true;
  r.begin_struct();
  while (bret) {
    uint8_t t = 0;
    int16_t id = 0;
    if (!r.next_field(t, id)) {
      bret = false;
    } else if (0 == t) {
      break;
    } else if (3 == id) {
      bret = pq_parse_col_meta(r, meta);
    } else {
      bret = r.skip(t);
    }
  }
  r.end_struct();
  return bret;
}

bool pq_parse_row_group(PqReader &r, PqRowGroupMeta &rg)
{
  bool bret = true;
  r.begin_struct();
  while (bret) {
    uint8_t t = 0;
    int16_t id = 0;
    if (!r.next_field(t, id)) {
      bret = false;
    } else if (0 == t) {
      break;
    } else if (1 == id) {
      uint8_t elem_type = 0;
      int64_t count = 0;
      bret = r.read_list(elem_type, count);
      for (int64_t i = 0; bret && i < count; ++i) {
        PqColMeta meta;
        if (!pq_parse_column_chunk(r, meta)) {
          bret = false;
        } else {
          rg.columns_.push_back(meta);
        }
      }
    } else if (3 == id) {
      bret = r.read_i64(rg.num_rows_);
    } else {
      bret = r.skip(t);
    }
  }
  r.end_struct();
  return bret;
}

bool pq_parse_file_meta(const char *data, const int64_t size, PqFileMeta &meta)
{
  bool bret = true;
  PqReader r(data, size);
  r.begin_struct();
  while (bret) {
    uint8_t t = 0;
    int16_t id = 0;
    if (!r.next_field(t, id)) {
      bret = false;
    } else if (0 == t) {
      break;
    } else if (2 == id) {
      uint8_t elem_type = 0;
      int64_t count = 0;
      bret = r.read_list(elem_type, count);
      for (int64_t i = 0; bret && i < count; ++i) {
        PqSchemaElem elem;
        if (!pq_parse_schema_element(r, elem)) {
          bret = false;
        } else {
          meta.schema_.push_back(elem);
        }
      }
    } else if (3 == id) {
      bret = r.read_i64(meta.num_rows_);
    } else if (4 == id) {
      uint8_t elem_type = 0;
      int64_t count = 0;
      bret = r.read_list(elem_type, count);
      for (int64_t i = 0; bret && i < count; ++i) {
        PqRowGroupMeta rg;
        if (!pq_parse_row_group(r, rg)) {
          bret = false;
        } else {
          meta.row_groups_.push_back(rg);
        }
      }
    } else {
      bret = r.skip(t);
    }
  }
  r.end_struct();
  return bret;
}

bool pq_parse_page_header(PqReader &r, PqPageHeader &h)
{
  bool bret = true;
  r.begin_struct();
  while (bret) {
    uint8_t t = 0;
    int16_t id = 0;
    if (!r.next_field(t, id)) {
      bret = false;
    } else if (0 == t) {
      break;
    } else if (1 == id) {
      bret = r.read_i32(h.type_);
    } else if (2 == id) {
      bret = r.read_i32(h.uncompressed_size_);
    } else if (3 == id) {
      bret = r.read_i32(h.compressed_size_);
    } else if (5 == id) {
      h.is_v2_ = false;
      r.begin_struct();
      while (bret) {
        uint8_t t2 = 0;
        int16_t id2 = 0;
        if (!r.next_field(t2, id2)) {
          bret = false;
        } else if (0 == t2) {
          break;
        } else if (1 == id2) {
          bret = r.read_i32(h.num_values_);
        } else if (2 == id2) {
          bret = r.read_i32(h.encoding_);
        } else {
          bret = r.skip(t2);
        }
      }
      r.end_struct();
    } else if (7 == id) {
      r.begin_struct();
      while (bret) {
        uint8_t t2 = 0;
        int16_t id2 = 0;
        if (!r.next_field(t2, id2)) {
          bret = false;
        } else if (0 == t2) {
          break;
        } else if (1 == id2) {
          bret = r.read_i32(h.num_dict_values_);
        } else if (2 == id2) {
          bret = r.read_i32(h.encoding_);
        } else {
          bret = r.skip(t2);
        }
      }
      r.end_struct();
    } else if (8 == id) {
      h.is_v2_ = true;
      r.begin_struct();
      while (bret) {
        uint8_t t2 = 0;
        int16_t id2 = 0;
        if (!r.next_field(t2, id2)) {
          bret = false;
        } else if (0 == t2) {
          break;
        } else if (1 == id2) {
          bret = r.read_i32(h.num_values_);
        } else if (4 == id2) {
          bret = r.read_i32(h.encoding_);
        } else if (5 == id2) {
          bret = r.read_i32(h.def_levels_byte_length_);
        } else if (7 == id2) {
          h.is_compressed_v2_ = (1 == t2);
        } else {
          bret = r.skip(t2);
        }
      }
      r.end_struct();
    } else {
      bret = r.skip(t);
    }
  }
  r.end_struct();
  return bret;
}

bool pq_snappy_uncompress(const char *in,
                          const int64_t in_len,
                          char *out,
                          const int64_t out_cap,
                          int64_t &out_len)
{
  bool bret = true;
  int64_t pos = 0;
  uint64_t ulen = 0;
  int shift = 0;
  while (bret) {
    if (pos >= in_len) {
      bret = false;
      break;
    }
    const uint8_t b = static_cast<uint8_t>(in[pos++]);
    ulen |= (static_cast<uint64_t>(b & 0x7f) << shift);
    shift += 7;
    if (0 == (b & 0x80)) {
      break;
    } else if (shift > 35) {
      bret = false;
    }
  }
  if (bret && static_cast<int64_t>(ulen) > out_cap) {
    bret = false;
  }
  if (bret) {
    int64_t op = 0;
    const int64_t target = static_cast<int64_t>(ulen);
    while (bret && pos < in_len) {
      const uint8_t tag = static_cast<uint8_t>(in[pos++]);
      const int type = tag & 0x03;
      if (0 == type) {
        int64_t len = (tag >> 2) + 1;
        if (len > 60) {
          const int64_t extra = len - 60;
          if (pos + extra > in_len) {
            bret = false;
            break;
          }
          len = 0;
          for (int64_t i = 0; i < extra; ++i) {
            len |= static_cast<int64_t>(static_cast<uint8_t>(in[pos + i])) << (8 * i);
          }
          pos += extra;
          len += 1;
        }
        if (pos + len > in_len || op + len > target) {
          bret = false;
          break;
        }
        memcpy(out + op, in + pos, len);
        pos += len;
        op += len;
      } else {
        int64_t len = 0;
        int64_t offset = 0;
        if (1 == type) {
          if (pos + 1 > in_len) {
            bret = false;
            break;
          }
          offset = (static_cast<int64_t>((tag >> 5) & 0x07) << 8)
                   | static_cast<int64_t>(static_cast<uint8_t>(in[pos]));
          len = ((tag >> 2) & 0x07) + 4;
          ++pos;
        } else if (2 == type) {
          if (pos + 2 > in_len) {
            bret = false;
            break;
          }
          len = (tag >> 2) + 1;
          offset = static_cast<int64_t>(static_cast<uint8_t>(in[pos]))
                   | (static_cast<int64_t>(static_cast<uint8_t>(in[pos + 1])) << 8);
          pos += 2;
        } else {
          if (pos + 4 > in_len) {
            bret = false;
            break;
          }
          len = (tag >> 2) + 1;
          offset = static_cast<int64_t>(static_cast<uint8_t>(in[pos]))
                   | (static_cast<int64_t>(static_cast<uint8_t>(in[pos + 1])) << 8)
                   | (static_cast<int64_t>(static_cast<uint8_t>(in[pos + 2])) << 16)
                   | (static_cast<int64_t>(static_cast<uint8_t>(in[pos + 3])) << 24);
          pos += 4;
        }
        if (offset <= 0 || offset > op || op + len > target) {
          bret = false;
          break;
        }
        for (int64_t i = 0; i < len; ++i) {
          out[op + i] = out[op - offset + i];
        }
        op += len;
      }
    }
    if (bret) {
      out_len = op;
      bret = (op == target);
    }
  }
  return bret;
}

bool pq_decompress(const int32_t codec,
                   const char *in,
                   const int64_t in_len,
                   char *out,
                   const int64_t out_cap,
                   int64_t &out_len)
{
  bool bret = true;
  if (0 == codec) {
    if (in_len > out_cap) {
      bret = false;
    } else {
      memcpy(out, in, in_len);
      out_len = in_len;
    }
  } else if (1 == codec) {
    bret = pq_snappy_uncompress(in, in_len, out, out_cap, out_len);
  } else {
    bret = false;
  }
  return bret;
}

int pq_bit_width(const int64_t max_value)
{
  int width = 0;
  int64_t v = max_value;
  while (v > 0) {
    ++width;
    v >>= 1;
  }
  return width;
}

bool pq_hybrid_decode(const char *p,
                      const int64_t avail,
                      const int64_t num_values,
                      const int bit_width,
                      std::vector<int64_t> &out,
                      int64_t &bytes_used)
{
  bool bret = true;
  int64_t pos = 0;
  int64_t produced = 0;
  out.assign(num_values, 0);
  while (bret && produced < num_values) {
    uint64_t header = 0;
    int shift = 0;
    while (bret) {
      if (pos >= avail) {
        bret = false;
        break;
      }
      const uint8_t b = static_cast<uint8_t>(p[pos++]);
      header |= (static_cast<uint64_t>(b & 0x7f) << shift);
      shift += 7;
      if (0 == (b & 0x80)) {
        break;
      } else if (shift > 63) {
        bret = false;
      }
    }
    if (!bret) {
      break;
    }
    if (header & 1) {
      const int64_t groups = static_cast<int64_t>(header >> 1);
      const int64_t bytes = groups * bit_width;
      if (pos + bytes > avail) {
        bret = false;
        break;
      }
      for (int64_t i = 0; i < groups * 8 && produced < num_values; ++i) {
        int64_t v = 0;
        for (int b = 0; b < bit_width; ++b) {
          const int64_t bit_pos = i * bit_width + b;
          const int64_t byte_idx = bit_pos / 8;
          const int bit_idx = bit_pos % 8;
          if (byte_idx < bytes) {
            v |= ((static_cast<int64_t>(static_cast<uint8_t>(p[pos + byte_idx])) >> bit_idx)
                  & 1LL)
                 << b;
          }
        }
        out[produced++] = v;
      }
      pos += bytes;
    } else {
      const int64_t run = static_cast<int64_t>(header >> 1);
      const int64_t width_bytes = (bit_width + 7) / 8;
      if (pos + width_bytes > avail) {
        bret = false;
        break;
      }
      int64_t v = 0;
      for (int64_t b = 0; b < width_bytes; ++b) {
        v |= static_cast<int64_t>(static_cast<uint8_t>(p[pos + b])) << (b * 8);
      }
      pos += width_bytes;
      for (int64_t i = 0; i < run && produced < num_values; ++i) {
        out[produced++] = v;
      }
    }
  }
  bytes_used = pos;
  return bret;
}

void pq_format_double(const double v, std::string &out)
{
  char buf[64];
  int n = 0;
  for (int prec = 15; prec <= 17; ++prec) {
    n = snprintf(buf, sizeof(buf), "%.*g", prec, v);
    if (n > 0 && strtod(buf, NULL) == v) {
      break;
    }
  }
  out.assign(buf, n > 0 ? n : 0);
}

bool pq_decode_plain(const int32_t type,
                     const char *p,
                     const int64_t len,
                     const int64_t count,
                     std::vector<std::string> &out)
{
  bool bret = true;
  if (count < 0) {
    bret = false;
  } else if (0 == type) {
    const int64_t need = (count + 7) / 8;
    if (need > len) {
      bret = false;
    } else {
      for (int64_t i = 0; i < count; ++i) {
        const bool v = ((static_cast<uint8_t>(p[i / 8]) >> (i % 8)) & 1) != 0;
        out.push_back(v ? std::string("1") : std::string("0"));
      }
    }
  } else if (1 == type) {
    if (len < count * 4) {
      bret = false;
    } else {
      for (int64_t i = 0; i < count; ++i) {
        int32_t v = 0;
        memcpy(&v, p + i * 4, 4);
        char buf[16];
        const int n = snprintf(buf, sizeof(buf), "%d", v);
        out.push_back(std::string(buf, n > 0 ? n : 0));
      }
    }
  } else if (2 == type) {
    if (len < count * 8) {
      bret = false;
    } else {
      for (int64_t i = 0; i < count; ++i) {
        int64_t v = 0;
        memcpy(&v, p + i * 8, 8);
        char buf[24];
        const int n = snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        out.push_back(std::string(buf, n > 0 ? n : 0));
      }
    }
  } else if (4 == type) {
    if (len < count * 4) {
      bret = false;
    } else {
      for (int64_t i = 0; i < count; ++i) {
        float v = 0.0f;
        memcpy(&v, p + i * 4, 4);
        std::string text;
        pq_format_double(static_cast<double>(v), text);
        out.push_back(text);
      }
    }
  } else if (5 == type) {
    if (len < count * 8) {
      bret = false;
    } else {
      for (int64_t i = 0; i < count; ++i) {
        double v = 0.0;
        memcpy(&v, p + i * 8, 8);
        std::string text;
        pq_format_double(v, text);
        out.push_back(text);
      }
    }
  } else if (6 == type) {
    int64_t off = 0;
    for (int64_t i = 0; bret && i < count; ++i) {
      if (off + 4 > len) {
        bret = false;
        break;
      }
      uint32_t l = 0;
      memcpy(&l, p + off, 4);
      off += 4;
      if (off + static_cast<int64_t>(l) > len) {
        bret = false;
        break;
      }
      out.push_back(std::string(p + off, l));
      off += static_cast<int64_t>(l);
    }
  } else {
    bret = false;
  }
  return bret;
}

bool pq_decode_dict_indices(const char *p,
                            const int64_t len,
                            const int64_t count,
                            const std::vector<std::string> &dict,
                            std::vector<std::string> &out)
{
  bool bret = true;
  if (len < 1) {
    bret = false;
  } else {
    const int bit_width = static_cast<int>(static_cast<uint8_t>(p[0]));
    std::vector<int64_t> idx;
    int64_t used = 0;
    if (!pq_hybrid_decode(p + 1, len - 1, count, bit_width, idx, used)) {
      bret = false;
    } else {
      for (int64_t i = 0; bret && i < count; ++i) {
        if (idx.at(i) < 0 || idx.at(i) >= static_cast<int64_t>(dict.size())) {
          bret = false;
        } else {
          out.push_back(dict.at(idx.at(i)));
        }
      }
    }
  }
  return bret;
}

bool pq_decode_values(const int32_t type,
                      const int32_t encoding,
                      const char *p,
                      const int64_t len,
                      const int64_t count,
                      const std::vector<std::string> &dict,
                      std::vector<std::string> &out)
{
  bool bret = false;
  if (0 == encoding) {
    bret = pq_decode_plain(type, p, len, count, out);
  } else if (2 == encoding || 8 == encoding) {
    bret = pq_decode_dict_indices(p, len, count, dict, out);
  }
  return bret;
}

bool pq_decode_column_pages(const char *file,
                            const int64_t size,
                            const PqColMeta &meta,
                            const int64_t max_def,
                            std::vector<std::string> &texts,
                            std::vector<bool> &nulls)
{
  bool bret = true;
  std::vector<std::string> dict;
  int64_t pos = (meta.dictionary_page_offset_ > 0 ? meta.dictionary_page_offset_
                                                 : meta.data_page_offset_);
  if (pos <= 0 || pos >= size) {
    bret = false;
  }
  int64_t value_cnt = 0;
  int64_t page_cnt = 0;
  while (bret && value_cnt < meta.num_values_) {
    if (++page_cnt > 1000000) {
      bret = false;
      break;
    }
    PqReader r(file, size);
    r.set_pos(pos);
    PqPageHeader h;
    if (!pq_parse_page_header(r, h)) {
      bret = false;
      break;
    }
    const int64_t header_len = r.pos() - pos;
    pos += header_len;
    if (h.compressed_size_ < 0 || pos + h.compressed_size_ > size) {
      bret = false;
      break;
    }
    const char *payload = file + pos;
    if (2 == h.type_) {
      std::vector<char> buf;
      const char *data = payload;
      int64_t data_len = h.compressed_size_;
      if (h.uncompressed_size_ > 0 && h.uncompressed_size_ != h.compressed_size_) {
        buf.resize(h.uncompressed_size_);
        if (!pq_decompress(meta.codec_, payload, h.compressed_size_, buf.data(),
                           h.uncompressed_size_, data_len)) {
          bret = false;
          break;
        }
        data = buf.data();
      }
      dict.clear();
      if (!pq_decode_plain(meta.type_, data, data_len, h.num_dict_values_, dict)) {
        bret = false;
        break;
      }
      pos += h.compressed_size_;
    } else if (0 == h.type_ || 3 == h.type_) {
      std::vector<char> buf;
      const char *data = payload;
      int64_t data_len = h.compressed_size_;
      int64_t def_byte_len = 0;
      const char *def_data = NULL;
      int64_t def_data_len = 0;
      const char *value_data = NULL;
      int64_t value_len = 0;
      if (!h.is_v2_) {
        if (h.uncompressed_size_ > 0 && h.uncompressed_size_ != h.compressed_size_) {
          buf.resize(h.uncompressed_size_);
          if (!pq_decompress(meta.codec_, payload, h.compressed_size_, buf.data(),
                             h.uncompressed_size_, data_len)) {
            bret = false;
            break;
          }
          data = buf.data();
        }
        def_byte_len = (max_def > 0 ? 4 : 0);
        if (data_len < def_byte_len) {
          bret = false;
          break;
        }
        if (max_def > 0) {
          uint32_t level_len = 0;
          memcpy(&level_len, data, 4);
          def_data = data + 4;
          def_data_len = static_cast<int64_t>(level_len);
          if (4 + def_data_len > data_len) {
            bret = false;
            break;
          }
        }
        value_data = data + def_byte_len + def_data_len;
        value_len = data_len - def_byte_len - def_data_len;
      } else {
        def_byte_len = h.def_levels_byte_length_;
        if (def_byte_len < 0 || def_byte_len > h.compressed_size_) {
          bret = false;
          break;
        }
        if (max_def > 0) {
          def_data = payload;
          def_data_len = def_byte_len;
        }
        const int64_t compressed_values_len = h.compressed_size_ - def_byte_len;
        if (h.is_compressed_v2_) {
          const int64_t uncompressed_values_len = h.uncompressed_size_ - def_byte_len;
          if (uncompressed_values_len < 0) {
            bret = false;
            break;
          }
          buf.resize(uncompressed_values_len);
          if (!pq_decompress(meta.codec_, payload + def_byte_len, compressed_values_len,
                             buf.data(), uncompressed_values_len, value_len)) {
            bret = false;
            break;
          }
          value_data = buf.data();
        } else {
          value_data = payload + def_byte_len;
          value_len = compressed_values_len;
        }
      }
      std::vector<int64_t> defs;
      int64_t used = 0;
      if (max_def > 0) {
        if (!pq_hybrid_decode(def_data, def_data_len, h.num_values_, pq_bit_width(max_def),
                              defs, used)) {
          bret = false;
          break;
        }
      } else {
        defs.assign(h.num_values_, 0);
      }
      int64_t value_in_page = 0;
      for (int64_t i = 0; i < h.num_values_; ++i) {
        if (max_def > 0 && defs.at(i) != max_def) {
          ++value_in_page;
        }
      }
      int64_t real_value_cnt = h.num_values_ - value_in_page;
      std::vector<std::string> values;
      if (real_value_cnt > 0) {
        if (!pq_decode_values(meta.type_, h.encoding_, value_data, value_len,
                              real_value_cnt, dict, values)) {
          bret = false;
          break;
        }
      }
      int64_t vi = 0;
      for (int64_t i = 0; i < h.num_values_; ++i) {
        if (max_def > 0 && defs.at(i) != max_def) {
          texts.push_back(std::string());
          nulls.push_back(true);
        } else {
          if (vi >= static_cast<int64_t>(values.size())) {
            bret = false;
            break;
          }
          texts.push_back(values.at(vi));
          nulls.push_back(false);
          ++vi;
        }
      }
      value_cnt += h.num_values_;
      pos += h.compressed_size_;
    } else {
      pos += h.compressed_size_;
    }
  }
  return bret && value_cnt == meta.num_values_;
}

ObFileSqlType pq_type_to_sql_type(const int32_t type)
{
  ObFileSqlType ret = FS_TYPE_VARCHAR;
  switch (type) {
    case 0:
      ret = FS_TYPE_BOOLEAN;
      break;
    case 1:
    case 2:
      ret = FS_TYPE_BIGINT;
      break;
    case 4:
    case 5:
      ret = FS_TYPE_DOUBLE;
      break;
    default:
      ret = FS_TYPE_VARCHAR;
      break;
  }
  return ret;
}

}  // namespace

int ObFileSqlReader::parse_parquet(const char *buf,
                                   const int64_t size,
                                   ObFileTableDef &def,
                                   ObIArray<ObFileSqlCell> &cells)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(size < 12 || 0 != memcmp(buf, "PAR1", 4)
                  || 0 != memcmp(buf + size - 4, "PAR1", 4))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("the file is not a parquet file", K(ret), K(def.path_));
  } else {
    uint32_t meta_len = 0;
    memcpy(&meta_len, buf + size - 8, 4);
    const int64_t meta_start = size - 8 - static_cast<int64_t>(meta_len);
    PqFileMeta meta;
    if (meta_start < 4 || meta_start >= size) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid parquet metadata", K(ret), K(def.path_), K(meta_len));
    } else if (!pq_parse_file_meta(buf + meta_start, static_cast<int64_t>(meta_len), meta)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("parse parquet metadata failed", K(ret), K(def.path_));
    } else if (meta.schema_.empty() || meta.schema_.at(0).num_children_ <= 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("unsupported parquet schema", K(ret), K(def.path_));
    } else {
      // Only flat schemas are supported, like the jsonl reader.
      const int64_t column_cnt = meta.schema_.at(0).num_children_;
      if (column_cnt > OB_FILE_MAX_COLUMN_CNT
          || (1 + column_cnt) > static_cast<int64_t>(meta.schema_.size())) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid parquet schema", K(ret), K(column_cnt));
      }
      std::vector<int32_t> col_types;
      std::vector<int64_t> col_max_def;
      std::vector<std::string> col_names;
      for (int64_t c = 0; OB_SUCC(ret) && c < column_cnt; ++c) {
        const PqSchemaElem &elem = meta.schema_.at(1 + c);
        if (elem.num_children_ > 0) {
          ret = OB_NOT_SUPPORTED;
          LOG_WARN("nested parquet columns are not supported", K(ret), K(c));
        } else {
          col_types.push_back(elem.type_);
          col_max_def.push_back(1 == elem.repetition_ ? 1 : 0);
          col_names.push_back(elem.name_);
        }
      }
      if (OB_SUCC(ret)) {
        def.column_cnt_ = column_cnt;
        def.row_cnt_ = meta.num_rows_;
        for (int64_t c = 0; c < column_cnt; ++c) {
          if (OB_FAIL(alloc_string(allocator_, col_names.at(c).data(),
                                   static_cast<int64_t>(col_names.at(c).size()),
                                   def.columns_[c].name_))) {
            LOG_WARN("copy column name failed", K(ret));
          } else {
            def.columns_[c].type_ = pq_type_to_sql_type(col_types.at(c));
            def.columns_[c].nullable_ = col_max_def.at(c) > 0;
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(dedup_column_names(def))) {
        LOG_WARN("dedup column names failed", K(ret));
      } else {
        std::vector<std::vector<std::string> > col_texts(column_cnt);
        std::vector<std::vector<bool> > col_nulls(column_cnt);
        int64_t total_rows = 0;
        for (int64_t rg = 0; OB_SUCC(ret) && rg < static_cast<int64_t>(meta.row_groups_.size()); ++rg) {
          const PqRowGroupMeta &row_group = meta.row_groups_.at(rg);
          if (static_cast<int64_t>(row_group.columns_.size()) != column_cnt) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("the column count of the row group is unexpected", K(ret), K(rg));
          }
          for (int64_t c = 0; OB_SUCC(ret) && c < column_cnt; ++c) {
            std::vector<std::string> texts;
            std::vector<bool> nulls;
            if (!pq_decode_column_pages(buf, size, row_group.columns_.at(c),
                                        col_max_def.at(c), texts, nulls)) {
              ret = OB_INVALID_ARGUMENT;
              LOG_WARN("decode parquet column failed", K(ret), K(rg), K(c), K(def.path_));
            } else if (static_cast<int64_t>(texts.size()) != row_group.num_rows_) {
              ret = OB_INVALID_ARGUMENT;
              LOG_WARN("unexpected value count", K(ret), K(rg), K(c),
                       K(texts.size()), K(row_group.num_rows_));
            } else {
              col_texts.at(c).insert(col_texts.at(c).end(), texts.begin(), texts.end());
              col_nulls.at(c).insert(col_nulls.at(c).end(), nulls.begin(), nulls.end());
            }
          }
          if (OB_SUCC(ret)) {
            total_rows += row_group.num_rows_;
          }
        }
        if (OB_SUCC(ret)) {
          if (total_rows > OB_FILE_SQL_MAX_ROW_CNT) {
            ret = OB_SIZE_OVERFLOW;
          } else if (meta.num_rows_ != total_rows) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("unexpected row count", K(ret), K(meta.num_rows_), K(total_rows));
          } else {
            def.row_cnt_ = total_rows;
            for (int64_t r = 0; OB_SUCC(ret) && r < total_rows; ++r) {
              for (int64_t c = 0; OB_SUCC(ret) && c < column_cnt; ++c) {
                ObFileSqlCell cell;
                if (col_nulls.at(c).at(r)) {
                  cell.is_null_ = true;
                } else if (OB_FAIL(alloc_string(allocator_, col_texts.at(c).at(r).data(),
                                                static_cast<int64_t>(
                                                    col_texts.at(c).at(r).size()),
                                                cell.text_))) {
                  LOG_WARN("copy value failed", K(ret));
                }
                if (OB_SUCC(ret) && OB_FAIL(cells.push_back(cell))) {
                  LOG_WARN("push back cell failed", K(ret));
                }
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
