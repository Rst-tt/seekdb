#pragma once

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_array.h"
#include "lib/ob_errno.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "common/object/ob_object.h"

#include "rapidjson/document.h"

namespace oceanbase
{
namespace sql
{

enum class ObFileSqlFormat : int8_t
{
  CSV = 0,
  JSONL = 1,
  NDJSON = 2,
  PARQUET = 3,
  GLOB = 4,
};

struct ObFileSqlColumn
{
  common::ObString name_;
  common::ObObjType type_;
  bool nullable_;
  int32_t precision_;
  int8_t scale_;
};

struct ObFileSqlOptions
{
  ObFileSqlOptions()
      : header_(true),
        delimiter_(','),
        quote_('"'),
        sample_size_(-1),
        max_file_size_(1024LL * 1024LL * 1024LL),
        max_rows_(10000000),
        null_text_()
  {
  }
  bool header_;
  char delimiter_;
  char quote_;
  int64_t sample_size_;
  int64_t max_file_size_;
  int64_t max_rows_;
  common::ObString null_text_;
};

class ObFileSqlUtils
{
public:
  static int infer_schema(common::ObIAllocator &allocator,
                          const common::ObString &path,
                          ObFileSqlFormat format,
                          const ObFileSqlOptions &options,
                          common::ObArray<ObFileSqlColumn> &columns,
                          common::ObString &error_msg)
  {
    int ret = common::OB_SUCCESS;
    if (format == ObFileSqlFormat::CSV) {
      ret = infer_csv_schema_(allocator, path, options, columns, error_msg);
    } else if (format == ObFileSqlFormat::JSONL || format == ObFileSqlFormat::NDJSON) {
      ret = infer_jsonl_schema_(allocator, path, options, columns, error_msg);
    } else if (format == ObFileSqlFormat::PARQUET) {
      ret = infer_parquet_schema_(allocator, path, columns, error_msg);
    } else {
      ret = common::OB_NOT_SUPPORTED;
      error_msg = common::ObString("glob schema inference is not implemented yet");
    }
    return ret;
  }

  static int expand_glob(const std::string &pattern, std::vector<std::string> &files);

private:
  enum class InferType : int8_t
  {
    UNKNOWN,
    BIGINT,
    DOUBLE,
    BOOLEAN,
    VARCHAR,
    DATE,
    DATETIME,
  };

  static bool is_null_value_(const std::string &value, const common::ObString &null_text)
  {
    return !null_text.empty() &&
           null_text.length() == static_cast<int32_t>(value.size()) &&
           memcmp(null_text.ptr(), value.data(), value.size()) == 0;
  }

  static bool is_integer_(const std::string &value)
  {
    if (value.empty()) {
      return false;
    }
    size_t pos = (value[0] == '-' || value[0] == '+') ? 1 : 0;
    if (pos >= value.size()) {
      return false;
    }
    for (size_t i = pos; i < value.size(); ++i) {
      if (value[i] < '0' || value[i] > '9') {
        return false;
      }
    }
    errno = 0;
    char *end = nullptr;
    strtoll(value.c_str(), &end, 10);
    return end != value.c_str() && *end == '\0' && ERANGE != errno;
  }

  static bool is_double_(const std::string &value)
  {
    if (value.empty()) {
      return false;
    }
    char *end = nullptr;
    strtod(value.c_str(), &end);
    return end != value.c_str() && *end == '\0';
  }

  static bool is_boolean_(const std::string &value)
  {
    return value == "true" || value == "false" || value == "TRUE" || value == "FALSE";
  }

  static int parse_two_digits_(const std::string &value, size_t pos)
  {
    if (pos + 1 >= value.size() ||
        value[pos] < '0' || value[pos] > '9' ||
        value[pos + 1] < '0' || value[pos + 1] > '9') {
      return -1;
    }
    return (value[pos] - '0') * 10 + (value[pos + 1] - '0');
  }

  static bool is_leap_year_(int year)
  {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  }

  static bool is_valid_date_part_(const std::string &value, size_t pos)
  {
    if (pos + 10 > value.size() ||
        value[pos + 4] != '-' || value[pos + 7] != '-') {
      return false;
    }
    int year = 0;
    for (size_t i = 0; i < 4; ++i) {
      char ch = value[pos + i];
      if (ch < '0' || ch > '9') {
        return false;
      }
      year = year * 10 + (ch - '0');
    }
    int month = parse_two_digits_(value, pos + 5);
    int day = parse_two_digits_(value, pos + 8);
    if (year <= 0 || month < 1 || month > 12 || day < 1) {
      return false;
    }
    static const int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max_day = DAYS[month - 1];
    if (2 == month && is_leap_year_(year)) {
      max_day = 29;
    }
    return day <= max_day;
  }

  static bool is_date_(const std::string &value)
  {
    return value.size() == 10 && is_valid_date_part_(value, 0);
  }

  static bool is_datetime_(const std::string &value)
  {
    if (value.size() != 19 || (value[10] != ' ' && value[10] != 'T') ||
        value[13] != ':' || value[16] != ':' ||
        !is_valid_date_part_(value, 0)) {
      return false;
    }
    int hour = parse_two_digits_(value, 11);
    int minute = parse_two_digits_(value, 14);
    int second = parse_two_digits_(value, 17);
    if (hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
      return false;
    }
    return true;
  }

  static InferType infer_type_(const std::string &value, const common::ObString &null_text)
  {
    if (is_null_value_(value, null_text)) {
      return InferType::UNKNOWN;
    } else if (is_boolean_(value)) {
      return InferType::BOOLEAN;
    } else if (is_integer_(value)) {
      return InferType::BIGINT;
    } else if (is_double_(value)) {
      return InferType::DOUBLE;
    } else if (is_date_(value)) {
      return InferType::DATE;
    } else if (is_datetime_(value)) {
      return InferType::DATETIME;
    } else {
      return InferType::VARCHAR;
    }
  }

  static InferType merge_type_(InferType lhs, InferType rhs)
  {
    if (lhs == InferType::UNKNOWN) {
      return rhs;
    }
    if (rhs == InferType::UNKNOWN) {
      return lhs;
    }
    if (lhs == rhs) {
      return lhs;
    }
    if (lhs == InferType::BIGINT && rhs == InferType::DOUBLE) {
      return InferType::DOUBLE;
    }
    if (lhs == InferType::DOUBLE && rhs == InferType::BIGINT) {
      return InferType::DOUBLE;
    }
    return InferType::VARCHAR;
  }

  static common::ObObjType to_obj_type_(InferType type)
  {
    switch (type) {
      case InferType::BIGINT:
        return common::ObIntType;
      case InferType::DOUBLE:
        return common::ObDoubleType;
      case InferType::BOOLEAN:
        return common::ObTinyIntType;
      case InferType::DATE:
        return common::ObDateType;
      case InferType::DATETIME:
        return common::ObDateTimeType;
      case InferType::VARCHAR:
      case InferType::UNKNOWN:
      default:
        return common::ObVarcharType;
    }
  }

  static bool file_column_name_equal_(const std::string &lhs,
                                      const std::string &rhs)
  {
    bool equal = lhs.size() == rhs.size();
    for (size_t i = 0; equal && i < lhs.size(); ++i) {
      equal = std::tolower(static_cast<unsigned char>(lhs[i])) ==
              std::tolower(static_cast<unsigned char>(rhs[i]));
    }
    return equal;
  }

  static void make_unique_csv_column_names_(std::vector<std::string> &names)
  {
    for (size_t i = 0; i < names.size(); ++i) {
      std::string base = names[i];
      std::string candidate = base;
      int64_t suffix = 2;
      bool duplicated = true;
      while (duplicated) {
        duplicated = false;
        for (size_t j = 0; !duplicated && j < i; ++j) {
          duplicated = file_column_name_equal_(names[j], candidate);
        }
        if (duplicated) {
          candidate = base + "__" + std::to_string(suffix++);
        }
      }
      names[i] = candidate;
    }
  }

  static int infer_csv_schema_(common::ObIAllocator &allocator,
                               const common::ObString &path,
                               const ObFileSqlOptions &options,
                               common::ObArray<ObFileSqlColumn> &columns,
                               common::ObString &error_msg)
  {
    int ret = common::OB_SUCCESS;
    std::ifstream in(std::string(path.ptr(), path.length()), std::ios::binary);
    if (!in.is_open()) {
      ret = common::OB_FILE_NOT_EXIST;
      error_msg = common::ObString("failed to open csv file");
      return ret;
    }
    struct stat file_stat;
    if (0 == stat(std::string(path.ptr(), path.length()).c_str(), &file_stat) &&
        options.max_file_size_ > 0 && file_stat.st_size > options.max_file_size_) {
      ret = common::OB_SIZE_OVERFLOW;
      error_msg = common::ObString("csv file exceeds max_file_size");
      return ret;
    }

    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    in.close();

    std::vector<std::vector<std::string>> records;
    std::vector<std::vector<bool>> quoted_records;
    if (OB_FAIL(parse_csv_records_(content, options, records, quoted_records))) {
      error_msg = common::ObString("malformed csv record");
      return ret;
    }
    if (records.empty()) {
      ret = common::OB_INVALID_DATA;
      error_msg = common::ObString("csv file is empty");
      return ret;
    }

    const int64_t column_count = records[0].size();
    if (column_count == 0) {
      ret = common::OB_INVALID_DATA;
      error_msg = common::ObString("csv header is empty");
      return ret;
    }

    std::vector<InferType> types(column_count, InferType::UNKNOWN);
    std::vector<std::string> names(column_count);
    for (int64_t i = 0; i < column_count; ++i) {
      names[i] = options.header_ ? records[0][i] : "c" + std::to_string(i + 1);
    }
    make_unique_csv_column_names_(names);

    const int64_t first_data_row = options.header_ ? 1 : 0;
    std::vector<bool> nullable(column_count, 0 < first_data_row &&
                                             first_data_row >=
                                                 static_cast<int64_t>(records.size()));
    for (int64_t r = first_data_row; OB_SUCC(ret) && r < static_cast<int64_t>(records.size()); ++r) {
      if (records[r].size() != static_cast<size_t>(column_count)) {
        ret = common::OB_INVALID_DATA;
        error_msg = common::ObString("csv row has a different number of fields");
        break;
      }
      for (int64_t c = 0; c < column_count; ++c) {
        const std::string &value = records[r][c];
        const bool is_null = !quoted_records[r][c] &&
                             (value.empty() || is_null_value_(value, options.null_text_));
        if (is_null) {
          nullable[c] = true;
        } else {
          types[c] = merge_type_(types[c], infer_type_(value, options.null_text_));
        }
      }
    }

    for (int64_t c = 0; OB_SUCC(ret) && c < column_count; ++c) {
      ObFileSqlColumn column;
      common::ObString name;
      if (OB_FAIL(ob_write_string(allocator, common::ObString(names[c].size(), names[c].data()), name))) {
      } else {
        column.name_ = name;
        column.type_ = to_obj_type_(types[c]);
        column.nullable_ = nullable[c];
        column.precision_ = -1;
        column.scale_ = -1;
        if (OB_FAIL(columns.push_back(column))) {
        }
      }
    }
    return ret;
  }

  static int infer_jsonl_schema_(common::ObIAllocator &allocator,
                                 const common::ObString &path,
                                 const ObFileSqlOptions &options,
                                 common::ObArray<ObFileSqlColumn> &columns,
                                 common::ObString &error_msg)
  {
    int ret = common::OB_SUCCESS;
    std::ifstream in(std::string(path.ptr(), path.length()), std::ios::binary);
    if (!in.is_open()) {
      ret = common::OB_FILE_NOT_EXIST;
      error_msg = common::ObString("failed to open jsonl file");
      return ret;
    }
    struct stat file_stat;
    if (0 == stat(std::string(path.ptr(), path.length()).c_str(), &file_stat) &&
        options.max_file_size_ > 0 && file_stat.st_size > options.max_file_size_) {
      ret = common::OB_SIZE_OVERFLOW;
      error_msg = common::ObString("jsonl file exceeds max_file_size");
      return ret;
    }

    std::vector<std::string> names;
    std::vector<InferType> types;
    std::string line;
    int64_t row_no = 0;
    while (OB_SUCC(ret) && std::getline(in, line)) {
      ++row_no;
      if (line.empty()) {
        continue;
      }
      rapidjson::Document doc;
      doc.Parse(line.c_str());
      if (doc.HasParseError() || !doc.IsObject()) {
        ret = common::OB_INVALID_DATA;
        error_msg = common::ObString("jsonl row is not a flat object");
        break;
      }
      for (rapidjson::Value::ConstMemberIterator it = doc.MemberBegin();
           it != doc.MemberEnd(); ++it) {
        std::string key = it->name.GetString();
        const rapidjson::Value &value = it->value;
        if (value.IsObject() || value.IsArray()) {
          ret = common::OB_NOT_SUPPORTED;
          error_msg = common::ObString("nested json objects/arrays are not supported");
          break;
        }
        int64_t idx = -1;
        for (int64_t i = 0; i < static_cast<int64_t>(names.size()); ++i) {
          if (names[i] == key) {
            idx = i;
            break;
          }
        }
        if (idx < 0) {
          idx = names.size();
          names.push_back(key);
          types.push_back(InferType::UNKNOWN);
        }
        InferType next = InferType::VARCHAR;
        if (value.IsNull()) {
          next = InferType::UNKNOWN;
        } else if (value.IsInt64()) {
          next = InferType::BIGINT;
        } else if (value.IsUint64()) {
          next = value.GetUint64() <= static_cast<uint64_t>(INT64_MAX)
                     ? InferType::BIGINT : InferType::DOUBLE;
        } else if (value.IsDouble()) {
          next = InferType::DOUBLE;
        } else if (value.IsBool()) {
          next = InferType::BOOLEAN;
        } else if (value.IsString()) {
          next = InferType::VARCHAR;
        }
        types[idx] = merge_type_(types[idx], next);
      }
    }
    in.close();

    if (OB_SUCC(ret) && names.empty()) {
      ret = common::OB_INVALID_DATA;
      error_msg = common::ObString("jsonl file is empty");
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(names.size()); ++i) {
      ObFileSqlColumn column;
      common::ObString name;
      if (OB_FAIL(ob_write_string(allocator, common::ObString(names[i].size(), names[i].data()), name))) {
      } else {
        column.name_ = name;
        column.type_ = to_obj_type_(types[i]);
        column.nullable_ = true;
        column.precision_ = -1;
        column.scale_ = -1;
        if (OB_FAIL(columns.push_back(column))) {
        }
      }
    }
    return ret;
  }

  class ParquetCompactReader
  {
  public:
    ParquetCompactReader(const char *data, int64_t len)
        : data_(reinterpret_cast<const uint8_t *>(data)),
          len_(len),
          pos_(0)
    {
    }

    int read_byte(uint8_t &value)
    {
      int ret = common::OB_SUCCESS;
      if (pos_ >= len_) {
        ret = common::OB_INVALID_DATA;
      } else {
        value = data_[pos_++];
      }
      return ret;
    }

    int read_varint(uint64_t &value)
    {
      int ret = common::OB_SUCCESS;
      value = 0;
      int shift = 0;
      while (OB_SUCC(ret)) {
        uint8_t byte = 0;
        if (shift >= 64) {
          ret = common::OB_INVALID_DATA;
        } else if (OB_FAIL(read_byte(byte))) {
        } else {
          value |= static_cast<uint64_t>(byte & 0x7f) << shift;
          if ((byte & 0x80) == 0) {
            break;
          }
          shift += 7;
        }
      }
      return ret;
    }

    int read_zigzag(int64_t &value)
    {
      int ret = common::OB_SUCCESS;
      uint64_t raw = 0;
      if (OB_FAIL(read_varint(raw))) {
      } else {
        value = static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
      }
      return ret;
    }

    int read_field_header(uint8_t &type, int16_t &field_id, int16_t &last_id)
    {
      int ret = common::OB_SUCCESS;
      uint8_t byte = 0;
      if (OB_FAIL(read_byte(byte))) {
      } else if (0 == byte) {
        type = 0;
      } else {
        type = byte & 0x0f;
        uint8_t delta = byte >> 4;
        if (0 == delta) {
          int64_t id = 0;
          if (OB_FAIL(read_zigzag(id))) {
          } else {
            field_id = static_cast<int16_t>(id);
          }
        } else {
          field_id = last_id + delta;
        }
        last_id = field_id;
      }
      return ret;
    }

    int read_binary(std::string &value)
    {
      int ret = common::OB_SUCCESS;
      uint64_t len = 0;
      if (OB_FAIL(read_varint(len)) || len > static_cast<uint64_t>(len_ - pos_)) {
        ret = common::OB_INVALID_DATA;
      } else {
        value.assign(reinterpret_cast<const char *>(data_ + pos_), len);
        pos_ += len;
      }
      return ret;
    }

    int read_list_header(uint8_t &elem_type, uint64_t &size)
    {
      int ret = common::OB_SUCCESS;
      uint8_t byte = 0;
      if (OB_FAIL(read_byte(byte))) {
      } else {
        elem_type = byte & 0x0f;
        size = byte >> 4;
        if (15 == size) {
          ret = read_varint(size);
        }
      }
      return ret;
    }

    int skip_value(uint8_t type)
    {
      int ret = common::OB_SUCCESS;
      if (1 == type || 2 == type) {
      } else if (3 == type) {
        uint8_t value = 0;
        ret = read_byte(value);
      } else if (4 == type || 5 == type || 6 == type) {
        int64_t value = 0;
        ret = read_zigzag(value);
      } else if (7 == type) {
        if (pos_ + 8 > len_) {
          ret = common::OB_INVALID_DATA;
        } else {
          pos_ += 8;
        }
      } else if (8 == type) {
        std::string value;
        ret = read_binary(value);
      } else if (9 == type || 10 == type) {
        uint8_t elem_type = 0;
        uint64_t size = 0;
        if (OB_FAIL(read_list_header(elem_type, size))) {
        } else {
          for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
            ret = skip_value(elem_type);
          }
        }
      } else if (11 == type) {
        uint64_t size = 0;
        uint8_t key_type = 0;
        uint8_t value_type = 0;
        if (OB_FAIL(read_varint(size))) {
        } else if (size > 0) {
          uint8_t byte = 0;
          if (OB_FAIL(read_byte(byte))) {
          } else {
            key_type = byte >> 4;
            value_type = byte & 0x0f;
            for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
              if (OB_FAIL(skip_value(key_type))) {
              } else {
                ret = skip_value(value_type);
              }
            }
          }
        }
      } else if (12 == type) {
        int16_t last_id = 0;
        while (OB_SUCC(ret)) {
          uint8_t field_type = 0;
          int16_t field_id = 0;
          if (OB_FAIL(read_field_header(field_type, field_id, last_id))) {
          } else if (0 == field_type) {
            break;
          } else {
            ret = skip_value(field_type);
          }
        }
      } else {
        ret = common::OB_INVALID_DATA;
      }
      return ret;
    }

  private:
    const uint8_t *data_;
    int64_t len_;
    int64_t pos_;
  };

  static int parquet_type_to_obj_type_(int64_t physical_type,
                                       int64_t converted_type,
                                       common::ObObjType &obj_type)
  {
    int ret = common::OB_SUCCESS;
    switch (physical_type) {
      case 0:
        obj_type = common::ObTinyIntType;
        break;
      case 1:
        obj_type = (6 == converted_type) ? common::ObDateType : common::ObIntType;
        break;
      case 2:
        obj_type = (9 == converted_type || 10 == converted_type) ?
                   common::ObDateTimeType : common::ObIntType;
        break;
      case 3:
        obj_type = common::ObDateTimeType;
        break;
      case 4:
        obj_type = common::ObFloatType;
        break;
      case 5:
        obj_type = common::ObDoubleType;
        break;
      case 6:
      case 7:
        obj_type = common::ObVarcharType;
        break;
      default:
        ret = common::OB_NOT_SUPPORTED;
        break;
    }
    return ret;
  }

  static int parse_parquet_schema_element_(ParquetCompactReader &reader,
                                           int64_t &physical_type,
                                           int64_t &converted_type,
                                           std::string &name,
                                           int64_t &num_children)
  {
    int ret = common::OB_SUCCESS;
    int16_t last_id = 0;
    physical_type = -1;
    converted_type = -1;
    num_children = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if ((1 == field_id || 3 == field_id || 5 == field_id || 6 == field_id) &&
                 (5 == type || 4 == type || 3 == type)) {
        int64_t value = 0;
        if (OB_FAIL(reader.read_zigzag(value))) {
        } else if (1 == field_id) {
          physical_type = value;
        } else if (5 == field_id) {
          num_children = value;
        } else if (6 == field_id) {
          converted_type = value;
        }
      } else if (4 == field_id && 8 == type) {
        if (OB_FAIL(reader.read_binary(name))) {
        }
      } else if (10 == field_id && 12 == type) {
        if (OB_FAIL(parse_parquet_logical_type_(reader, converted_type))) {
        }
      } else {
        ret = reader.skip_value(type);
      }
    }
    return ret;
  }

  static int parse_parquet_logical_type_(ParquetCompactReader &reader,
                                         int64_t &converted_type)
  {
    int ret = common::OB_SUCCESS;
    int16_t last_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if (6 == field_id && 12 == type) {
        converted_type = 6;
        ret = reader.skip_value(12);
      } else if (8 == field_id && 12 == type) {
        converted_type = 10;
        ret = reader.skip_value(12);
      } else {
        ret = reader.skip_value(type);
      }
    }
    return ret;
  }

  static int parse_parquet_footer_(ParquetCompactReader &reader,
                                   common::ObIAllocator &allocator,
                                   common::ObArray<ObFileSqlColumn> &columns,
                                   common::ObString &error_msg)
  {
    int ret = common::OB_SUCCESS;
    int16_t last_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if (2 == field_id && 9 == type) {
        uint8_t elem_type = 0;
        uint64_t schema_count = 0;
        if (OB_FAIL(reader.read_list_header(elem_type, schema_count))) {
        } else if (12 != elem_type) {
          ret = common::OB_INVALID_DATA;
        } else {
          for (uint64_t i = 0; OB_SUCC(ret) && i < schema_count; ++i) {
            int64_t physical_type = -1;
            int64_t converted_type = -1;
            int64_t num_children = 0;
            std::string name;
            if (OB_FAIL(parse_parquet_schema_element_(reader, physical_type,
                                                       converted_type, name,
                                                       num_children))) {
            } else if (physical_type < 0) {
              // group/root node
            } else {
              ObFileSqlColumn column;
              common::ObString column_name;
              if (OB_FAIL(parquet_type_to_obj_type_(physical_type, converted_type,
                                                     column.type_))) {
                error_msg = common::ObString("unsupported parquet physical type");
              } else if (OB_FAIL(ob_write_string(
                             allocator,
                             common::ObString(name.size(), name.data()),
                             column_name))) {
              } else {
                column.name_ = column_name;
                column.nullable_ = true;
                column.precision_ = -1;
                column.scale_ = -1;
                if (OB_FAIL(columns.push_back(column))) {
                }
              }
            }
          }
        }
      } else {
        ret = reader.skip_value(type);
      }
    }
    return ret;
  }

  static int infer_parquet_schema_(common::ObIAllocator &allocator,
                                   const common::ObString &path,
                                   common::ObArray<ObFileSqlColumn> &columns,
                                   common::ObString &error_msg)
  {
    int ret = common::OB_SUCCESS;
    std::ifstream in(std::string(path.ptr(), path.length()), std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
      ret = common::OB_FILE_NOT_EXIST;
      error_msg = common::ObString("failed to open parquet file");
    } else {
      std::streamoff file_size = in.tellg();
      if (file_size < 12) {
        ret = common::OB_INVALID_DATA;
        error_msg = common::ObString("parquet file is too small");
      } else {
        char magic[4] = {0};
        in.seekg(0, std::ios::beg);
        in.read(magic, 4);
        if (0 != memcmp(magic, "PAR1", 4)) {
          ret = common::OB_INVALID_DATA;
          error_msg = common::ObString("invalid parquet magic");
        } else {
          in.seekg(file_size - 8, std::ios::beg);
          uint8_t footer_len_buf[4] = {0};
          in.read(reinterpret_cast<char *>(footer_len_buf), 4);
          uint32_t footer_len = static_cast<uint8_t>(footer_len_buf[0]) |
                                (static_cast<uint32_t>(footer_len_buf[1]) << 8) |
                                (static_cast<uint32_t>(footer_len_buf[2]) << 16) |
                                (static_cast<uint32_t>(footer_len_buf[3]) << 24);
          char end_magic[4] = {0};
          in.read(end_magic, 4);
          if (0 != memcmp(end_magic, "PAR1", 4) ||
              static_cast<std::streamoff>(footer_len) > file_size - 12) {
            ret = common::OB_INVALID_DATA;
            error_msg = common::ObString("invalid parquet footer");
          } else {
            std::string footer(footer_len, '\0');
            in.seekg(file_size - 8 - footer_len, std::ios::beg);
            in.read(&footer[0], footer_len);
            ParquetCompactReader reader(footer.data(), footer.size());
            if (OB_FAIL(parse_parquet_footer_(reader, allocator, columns, error_msg))) {
            }
          }
        }
      }
    }
    in.close();
    return ret;
  }

  static int parse_csv_records_(const std::string &content,
                                const ObFileSqlOptions &options,
                                std::vector<std::vector<std::string>> &records,
                                std::vector<std::vector<bool>> &quoted_records)
  {
    int ret = common::OB_SUCCESS;
    std::vector<std::string> record;
    std::vector<bool> quoted_record;
    std::string field;
    bool in_quotes = false;
    bool field_quoted = false;
    auto finish_field = [&]() {
      record.push_back(field);
      quoted_record.push_back(field_quoted);
      field.clear();
      field_quoted = false;
    };
    auto finish_record = [&]() {
      finish_field();
      records.push_back(record);
      quoted_records.push_back(quoted_record);
      record.clear();
      quoted_record.clear();
    };
    const int64_t len = content.size();
    for (int64_t i = 0; i < len; ++i) {
      char ch = content[i];
      if (in_quotes) {
        if (ch == options.quote_) {
          if (i + 1 < len && content[i + 1] == options.quote_) {
            field.push_back(options.quote_);
            ++i;
          } else {
            in_quotes = false;
          }
        } else {
          field.push_back(ch);
        }
      } else if (ch == options.quote_) {
        in_quotes = true;
        field_quoted = true;
      } else if (ch == options.delimiter_) {
        finish_field();
      } else if (ch == '\n') {
        finish_record();
      } else if (ch == '\r') {
        if (i + 1 < len && content[i + 1] == '\n') {
          continue;
        } else {
          finish_record();
        }
      } else {
        field.push_back(ch);
      }
    }
    if (in_quotes) {
      ret = common::OB_INVALID_DATA;
    } else if (!field.empty() || !record.empty() || !quoted_record.empty() || field_quoted) {
      finish_record();
    }
    return ret;
  }
};

class ObFileCsvReader
{
public:
  ObFileCsvReader()
      : in_(),
        options_(),
        peek_(-1),
        eof_(true),
        stat_valid_(false),
        stat_info_(),
        row_count_(0)
  {
  }

  ~ObFileCsvReader()
  {
    if (in_.is_open()) {
      in_.close();
    }
  }

  int open(const common::ObString &path, const ObFileSqlOptions &options)
  {
    int ret = common::OB_SUCCESS;
    options_ = options;
    peek_ = -1;
    eof_ = false;
    stat_valid_ = false;
    row_count_ = 0;
    in_.clear();
    std::string path_str(path.ptr(), path.length());
    path_ = path_str;
    in_.open(path_str, std::ios::binary);
    if (!in_.is_open()) {
      ret = common::OB_FILE_NOT_EXIST;
      eof_ = true;
    } else if (0 == stat(path_str.c_str(), &stat_info_)) {
      stat_valid_ = true;
      if (options_.max_file_size_ > 0 && stat_info_.st_size > options_.max_file_size_) {
        ret = common::OB_SIZE_OVERFLOW;
        eof_ = true;
      }
    }
    if (OB_SUCC(ret) && options_.header_ && OB_FAIL(skip_header_())) {
      eof_ = true;
    }
    return ret;
  }

  int reset()
  {
    int ret = common::OB_SUCCESS;
    in_.clear();
    in_.seekg(0, std::ios::beg);
    peek_ = -1;
    eof_ = !in_.is_open();
    row_count_ = 0;
    if (OB_SUCC(ret) && !eof_ && options_.header_ && OB_FAIL(skip_header_())) {
      eof_ = true;
    }
    return ret;
  }

  int next(std::vector<std::string> &record)
  {
    std::vector<bool> nulls;
    return next(record, nulls);
  }

  int next(std::vector<std::string> &record, std::vector<bool> &nulls)
  {
    int ret = common::OB_SUCCESS;
    record.clear();
    nulls.clear();
    if (eof_) {
      ret = common::OB_ITER_END;
    } else if (options_.max_rows_ > 0 && row_count_ >= options_.max_rows_) {
      ret = common::OB_SIZE_OVERFLOW;
    } else if (stat_valid_ && OB_FAIL(check_unchanged_())) {
    } else {
      std::string field;
      bool in_quotes = false;
      bool field_quoted = false;
      bool any = false;
      std::vector<bool> quoted;
      while (true) {
        char ch = '\0';
        int next_ret = next_char_(ch);
        if (common::OB_SUCCESS != next_ret) {
          if (common::OB_ITER_END == next_ret) {
            if (in_quotes) {
              ret = common::OB_INVALID_DATA;
            } else if (any || !field.empty() || !record.empty()) {
              record.push_back(field);
              quoted.push_back(field_quoted);
              ret = common::OB_SUCCESS;
            } else {
              ret = common::OB_ITER_END;
              eof_ = true;
            }
          } else {
            ret = next_ret;
          }
          break;
        }
        any = true;
        if (in_quotes) {
          if (ch == options_.quote_) {
            char next = '\0';
            int next_ret = next_char_(next);
            if (common::OB_SUCCESS == next_ret && next == options_.quote_) {
              field.push_back(options_.quote_);
            } else {
              if (common::OB_SUCCESS == next_ret) {
                unget_char_(next);
              }
              in_quotes = false;
            }
          } else {
            field.push_back(ch);
          }
        } else if (ch == options_.quote_) {
          in_quotes = true;
          field_quoted = true;
        } else if (ch == options_.delimiter_) {
          record.push_back(field);
          quoted.push_back(field_quoted);
          field.clear();
          field_quoted = false;
        } else if (ch == '\n') {
          record.push_back(field);
          quoted.push_back(field_quoted);
          break;
        } else if (ch == '\r') {
          char next = '\0';
          int next_ret = next_char_(next);
          if (common::OB_SUCCESS == next_ret && next != '\n') {
            unget_char_(next);
          }
          record.push_back(field);
          quoted.push_back(field_quoted);
          break;
        } else {
          field.push_back(ch);
        }
      }
      if (OB_SUCC(ret)) {
        nulls.resize(record.size(), false);
        for (int64_t i = 0; i < static_cast<int64_t>(record.size()); ++i) {
          nulls[i] = is_null_field_(record[i], quoted[i]);
        }
        ++row_count_;
      }
    }
    return ret;
  }

private:
  bool is_null_field_(const std::string &value, bool quoted) const
  {
    return !quoted &&
           (value.empty() ||
            (!options_.null_text_.empty() &&
             options_.null_text_.length() == static_cast<int32_t>(value.size()) &&
             memcmp(options_.null_text_.ptr(), value.data(), value.size()) == 0));
  }

  int skip_header_()
  {
    int ret = common::OB_SUCCESS;
    std::vector<std::string> header;
    ret = next(header);
    if (common::OB_ITER_END == ret) {
      eof_ = true;
      ret = common::OB_SUCCESS;
    } else if (OB_SUCC(ret)) {
      row_count_ = 0;
    }
    return ret;
  }

  int next_char_(char &ch)
  {
    int ret = common::OB_SUCCESS;
    if (peek_ >= 0) {
      ch = static_cast<char>(peek_);
      peek_ = -1;
    } else {
      int c = in_.get();
      if (c == std::char_traits<char>::eof()) {
        ret = common::OB_ITER_END;
      } else {
        ch = static_cast<char>(c);
      }
    }
    return ret;
  }

  void unget_char_(char ch)
  {
    peek_ = static_cast<unsigned char>(ch);
  }

private:
  int check_unchanged_()
  {
    int ret = common::OB_SUCCESS;
    struct stat current;
    if (0 != stat(path_.c_str(), &current)) {
      ret = common::OB_FILE_NOT_EXIST;
    } else if (current.st_size != stat_info_.st_size ||
               current.st_mtime != stat_info_.st_mtime) {
      ret = common::OB_IO_ERROR;
    }
    return ret;
  }

private:
  std::ifstream in_;
  ObFileSqlOptions options_;
  int peek_;
  bool eof_;
  bool stat_valid_;
  struct stat stat_info_;
  std::string path_;
  int64_t row_count_;
};

class ObFileJsonlReader
{
public:
  ObFileJsonlReader()
      : in_(),
        columns_(),
        eof_(true),
        stat_valid_(false),
        stat_info_(),
        path_(),
        row_count_(0),
        max_file_size_(0),
        max_rows_(0)
  {
  }

  ~ObFileJsonlReader()
  {
    if (in_.is_open()) {
      in_.close();
    }
  }

  int open(const common::ObString &path, const ObFileSqlOptions &options)
  {
    int ret = common::OB_SUCCESS;
    columns_.clear();
    eof_ = false;
    stat_valid_ = false;
    row_count_ = 0;
    max_file_size_ = options.max_file_size_;
    max_rows_ = options.max_rows_;
    in_.clear();
    std::string path_str(path.ptr(), path.length());
    path_ = path_str;
    in_.open(path_str, std::ios::binary);
    if (!in_.is_open()) {
      ret = common::OB_FILE_NOT_EXIST;
      eof_ = true;
    } else if (0 == stat(path_str.c_str(), &stat_info_)) {
      stat_valid_ = true;
      if (max_file_size_ > 0 && stat_info_.st_size > max_file_size_) {
        ret = common::OB_SIZE_OVERFLOW;
        eof_ = true;
      }
    } else {
      ret = common::OB_IO_ERROR;
      eof_ = true;
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(collect_columns_())) {
        eof_ = true;
      } else if (OB_FAIL(reset())) {
        eof_ = true;
      }
    }
    return ret;
  }

  int reset()
  {
    int ret = common::OB_SUCCESS;
    in_.clear();
    in_.seekg(0, std::ios::beg);
    eof_ = !in_.is_open();
    row_count_ = 0;
    return ret;
  }

  const std::vector<std::string> &columns() const
  {
    return columns_;
  }

  int next(std::vector<std::string> &values, std::vector<bool> &nulls)
  {
    int ret = common::OB_SUCCESS;
    values.assign(columns_.size(), "");
    nulls.assign(columns_.size(), true);
    if (eof_) {
      ret = common::OB_ITER_END;
    } else if (max_rows_ > 0 && row_count_ >= max_rows_) {
      ret = common::OB_SIZE_OVERFLOW;
    } else if (stat_valid_ && OB_FAIL(check_unchanged_())) {
    } else {
      std::string line;
      bool got_line = false;
      while (OB_SUCC(ret) && std::getline(in_, line)) {
        if (line.empty()) {
          continue;
        }
        got_line = true;
        rapidjson::Document doc;
        doc.Parse(line.c_str());
        if (doc.HasParseError() || !doc.IsObject()) {
          ret = common::OB_INVALID_DATA;
          break;
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(columns_.size()); ++i) {
          rapidjson::Value::ConstMemberIterator it = doc.FindMember(columns_[i].c_str());
          if (it == doc.MemberEnd()) {
            nulls[i] = true;
            values[i] = "";
          } else if (it->value.IsNull()) {
            nulls[i] = true;
            values[i] = "";
          } else if (it->value.IsBool()) {
            nulls[i] = false;
            values[i] = it->value.GetBool() ? "true" : "false";
          } else if (it->value.IsInt64()) {
            nulls[i] = false;
            values[i] = std::to_string(it->value.GetInt64());
          } else if (it->value.IsUint64()) {
            nulls[i] = false;
            uint64_t value = it->value.GetUint64();
            if (value <= static_cast<uint64_t>(INT64_MAX)) {
              values[i] = std::to_string(static_cast<int64_t>(value));
            } else {
              char buf[32] = {0};
              snprintf(buf, sizeof(buf), "%.17g", static_cast<double>(value));
              values[i] = buf;
            }
          } else if (it->value.IsDouble()) {
            nulls[i] = false;
            values[i] = std::to_string(it->value.GetDouble());
          } else if (it->value.IsString()) {
            nulls[i] = false;
            values[i] = it->value.GetString();
          } else {
            ret = common::OB_NOT_SUPPORTED;
          }
        }
        break;
      }
      if (OB_SUCC(ret) && !got_line) {
        ret = common::OB_ITER_END;
        eof_ = true;
      } else if (OB_SUCC(ret)) {
        ++row_count_;
      }
    }
    return ret;
  }

private:
  int collect_columns_()
  {
    int ret = common::OB_SUCCESS;
    std::string line;
    while (OB_SUCC(ret) && std::getline(in_, line)) {
      if (line.empty()) {
        continue;
      }
      rapidjson::Document doc;
      doc.Parse(line.c_str());
      if (doc.HasParseError() || !doc.IsObject()) {
        ret = common::OB_INVALID_DATA;
        break;
      }
      for (rapidjson::Value::ConstMemberIterator it = doc.MemberBegin();
           it != doc.MemberEnd(); ++it) {
        const std::string key = it->name.GetString();
        if (it->value.IsObject() || it->value.IsArray()) {
          ret = common::OB_NOT_SUPPORTED;
          break;
        }
        if (std::find(columns_.begin(), columns_.end(), key) == columns_.end()) {
          columns_.push_back(key);
        }
      }
    }
    if (OB_SUCC(ret) && in_.bad()) {
      ret = common::OB_IO_ERROR;
    }
    return ret;
  }

private:
  int check_unchanged_()
  {
    int ret = common::OB_SUCCESS;
    struct stat current;
    if (0 != stat(path_.c_str(), &current)) {
      ret = common::OB_FILE_NOT_EXIST;
    } else if (current.st_size != stat_info_.st_size ||
               current.st_mtime != stat_info_.st_mtime) {
      ret = common::OB_IO_ERROR;
    }
    return ret;
  }

private:
  std::ifstream in_;
  std::vector<std::string> columns_;
  bool eof_;
  bool stat_valid_;
  struct stat stat_info_;
  std::string path_;
  int64_t row_count_;
  int64_t max_file_size_;
  int64_t max_rows_;
};

inline bool snappy_read_varint_(const uint8_t *input,
                                size_t input_len,
                                size_t &pos,
                                uint64_t &value)
{
  value = 0;
  int shift = 0;
  while (pos < input_len && shift < 64) {
    uint8_t byte = input[pos++];
    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      return true;
    }
    shift += 7;
  }
  return false;
}

inline int snappy_decompress(const char *input,
                             const size_t input_len,
                             std::vector<char> &output)
{
  int ret = common::OB_SUCCESS;
  output.clear();
  if (OB_ISNULL(input) || input_len == 0) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    const uint8_t *in = reinterpret_cast<const uint8_t *>(input);
    size_t pos = 0;
    uint64_t uncompressed_len = 0;
    if (!snappy_read_varint_(in, input_len, pos, uncompressed_len) ||
        uncompressed_len > static_cast<uint64_t>(INT64_MAX)) {
      ret = common::OB_INVALID_DATA;
    } else {
      output.reserve(static_cast<size_t>(uncompressed_len));
      while (OB_SUCC(ret) && pos < input_len &&
             output.size() < static_cast<size_t>(uncompressed_len)) {
        const uint8_t tag = in[pos++];
        const uint8_t type = tag & 0x03;
        if (0 == type) {
          uint64_t literal_len = (tag >> 2);
          if (literal_len < 60) {
            literal_len += 1;
          } else {
            const uint64_t extra_len = literal_len - 59;
            literal_len = 0;
            if (extra_len > 4 || pos + extra_len > input_len) {
              ret = common::OB_INVALID_DATA;
            } else {
              for (uint64_t i = 0; i < extra_len; ++i) {
                literal_len |= static_cast<uint64_t>(in[pos++]) << (8 * i);
              }
              literal_len += 1;
            }
          }
          if (OB_SUCC(ret) && (pos + literal_len > input_len ||
                               output.size() + literal_len > uncompressed_len)) {
            ret = common::OB_INVALID_DATA;
          } else if (OB_SUCC(ret)) {
            output.insert(output.end(), input + pos, input + pos + literal_len);
            pos += literal_len;
          }
        } else {
          uint64_t copy_len = 0;
          uint64_t offset = 0;
          if (1 == type) {
            if (pos >= input_len) {
              ret = common::OB_INVALID_DATA;
            } else {
              copy_len = ((tag >> 2) & 0x07) + 4;
              offset = ((tag & 0xe0) << 3) | in[pos++];
            }
          } else if (2 == type) {
            if (pos + 2 > input_len) {
              ret = common::OB_INVALID_DATA;
            } else {
              copy_len = (tag >> 2) + 1;
              offset = static_cast<uint64_t>(in[pos]) |
                       (static_cast<uint64_t>(in[pos + 1]) << 8);
              pos += 2;
            }
          } else {
            if (pos + 4 > input_len) {
              ret = common::OB_INVALID_DATA;
            } else {
              copy_len = (tag >> 2) + 1;
              offset = static_cast<uint64_t>(in[pos]) |
                       (static_cast<uint64_t>(in[pos + 1]) << 8) |
                       (static_cast<uint64_t>(in[pos + 2]) << 16) |
                       (static_cast<uint64_t>(in[pos + 3]) << 24);
              pos += 4;
            }
          }
          if (OB_SUCC(ret)) {
            if (0 == offset || offset > output.size() ||
                output.size() + copy_len > uncompressed_len) {
              ret = common::OB_INVALID_DATA;
            } else {
              const size_t copy_start = output.size() - static_cast<size_t>(offset);
              for (uint64_t i = 0; i < copy_len; ++i) {
                output.push_back(output[copy_start + i]);
              }
            }
          }
        }
      }
      if (OB_SUCC(ret) && output.size() != static_cast<size_t>(uncompressed_len)) {
        ret = common::OB_INVALID_DATA;
      }
    }
  }
  return ret;
}

class ObParquetMemorySource
{
public:
  ObParquetMemorySource()
      : data_(nullptr),
        len_(0),
        pos_(0)
  {
  }
  ObParquetMemorySource(const char *data, int64_t len)
      : data_(reinterpret_cast<const uint8_t *>(data)),
        len_(len),
        pos_(0)
  {
  }
  int read_byte(uint8_t &value)
  {
    int ret = common::OB_SUCCESS;
    if (pos_ >= len_) {
      ret = common::OB_INVALID_DATA;
    } else {
      value = data_[pos_++];
    }
    return ret;
  }
  int64_t position() const { return pos_; }
private:
  const uint8_t *data_;
  int64_t len_;
  int64_t pos_;
};

class ObParquetFileSource
{
public:
  ObParquetFileSource()
      : in_(nullptr),
        limit_(0),
        pos_(0)
  {
  }
  ObParquetFileSource(std::ifstream &in, int64_t limit)
      : in_(&in),
        limit_(limit),
        pos_(0)
  {
  }
  int read_byte(uint8_t &value)
  {
    int ret = common::OB_SUCCESS;
    if (OB_ISNULL(in_) || pos_ >= limit_) {
      ret = common::OB_INVALID_DATA;
    } else {
      char ch = '\0';
      in_->get(ch);
      if (!*in_) {
        ret = common::OB_IO_ERROR;
      } else {
        value = static_cast<uint8_t>(ch);
        ++pos_;
      }
    }
    return ret;
  }
  int64_t position() const { return pos_; }
private:
  std::ifstream *in_;
  int64_t limit_;
  int64_t pos_;
};

template <typename Source>
class ObParquetThriftReader
{
public:
  explicit ObParquetThriftReader(Source source)
      : source_(source)
  {
  }

  int read_byte(uint8_t &value)
  {
    return source_.read_byte(value);
  }

  int64_t position() const
  {
    return source_.position();
  }

  int read_varint(uint64_t &value)
  {
    int ret = common::OB_SUCCESS;
    value = 0;
    int shift = 0;
    while (OB_SUCC(ret)) {
      uint8_t byte = 0;
      if (shift >= 64) {
        ret = common::OB_INVALID_DATA;
      } else if (OB_FAIL(read_byte(byte))) {
      } else {
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
          break;
        }
        shift += 7;
      }
    }
    return ret;
  }

  int read_zigzag(int64_t &value)
  {
    int ret = common::OB_SUCCESS;
    uint64_t raw = 0;
    if (OB_FAIL(read_varint(raw))) {
    } else {
      value = static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
    }
    return ret;
  }

  int read_field_header(uint8_t &type, int16_t &field_id, int16_t &last_id)
  {
    int ret = common::OB_SUCCESS;
    uint8_t byte = 0;
    if (OB_FAIL(read_byte(byte))) {
    } else if (0 == byte) {
      type = 0;
    } else {
      type = byte & 0x0f;
      uint8_t delta = byte >> 4;
      if (0 == delta) {
        int64_t id = 0;
        if (OB_FAIL(read_zigzag(id))) {
        } else {
          field_id = static_cast<int16_t>(id);
        }
      } else {
        field_id = last_id + delta;
      }
      last_id = field_id;
    }
    return ret;
  }

  int read_binary(std::string &value)
  {
    int ret = common::OB_SUCCESS;
    uint64_t len = 0;
    if (OB_FAIL(read_varint(len))) {
    } else {
      value.clear();
      value.reserve(static_cast<size_t>(len));
      for (uint64_t i = 0; OB_SUCC(ret) && i < len; ++i) {
        uint8_t byte = 0;
        if (OB_FAIL(read_byte(byte))) {
        } else {
          value.push_back(static_cast<char>(byte));
        }
      }
    }
    return ret;
  }

  int read_list_header(uint8_t &elem_type, uint64_t &size)
  {
    int ret = common::OB_SUCCESS;
    uint8_t byte = 0;
    if (OB_FAIL(read_byte(byte))) {
    } else {
      elem_type = byte & 0x0f;
      size = byte >> 4;
      if (15 == size) {
        ret = read_varint(size);
      }
    }
    return ret;
  }

  int skip_value(uint8_t type)
  {
    int ret = common::OB_SUCCESS;
    if (1 == type || 2 == type) {
    } else if (3 == type) {
      uint8_t value = 0;
      ret = read_byte(value);
    } else if (4 == type || 5 == type || 6 == type) {
      int64_t value = 0;
      ret = read_zigzag(value);
    } else if (7 == type) {
      for (int i = 0; OB_SUCC(ret) && i < 8; ++i) {
        uint8_t value = 0;
        ret = read_byte(value);
      }
    } else if (8 == type) {
      std::string value;
      ret = read_binary(value);
    } else if (9 == type || 10 == type) {
      uint8_t elem_type = 0;
      uint64_t size = 0;
      if (OB_FAIL(read_list_header(elem_type, size))) {
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          ret = skip_value(elem_type);
        }
      }
    } else if (11 == type) {
      uint64_t size = 0;
      if (OB_FAIL(read_varint(size))) {
      } else if (size > 0) {
        uint8_t byte = 0;
        if (OB_FAIL(read_byte(byte))) {
        } else {
          uint8_t key_type = byte >> 4;
          uint8_t value_type = byte & 0x0f;
          for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
            if (OB_FAIL(skip_value(key_type))) {
            } else {
              ret = skip_value(value_type);
            }
          }
        }
      }
    } else if (12 == type) {
      int16_t last_id = 0;
      while (OB_SUCC(ret)) {
        uint8_t field_type = 0;
        int16_t field_id = 0;
        if (OB_FAIL(read_field_header(field_type, field_id, last_id))) {
        } else if (0 == field_type) {
          break;
        } else {
          ret = skip_value(field_type);
        }
      }
    } else {
      ret = common::OB_INVALID_DATA;
    }
    return ret;
  }

private:
  Source source_;
};

inline bool ob_parquet_read_varint_(const uint8_t *data,
                                    int64_t len,
                                    int64_t &pos,
                                    uint64_t &value)
{
  value = 0;
  int shift = 0;
  while (pos < len && shift < 64) {
    uint8_t byte = data[pos++];
    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      return true;
    }
    shift += 7;
  }
  return false;
}

inline int ob_parquet_extract_bits_(const uint8_t *data,
                                    int64_t data_len,
                                    int64_t bit_offset,
                                    int bit_width,
                                    uint64_t &value)
{
  int ret = common::OB_SUCCESS;
  value = 0;
  if (bit_width < 0 || bit_width > 64) {
    ret = common::OB_INVALID_DATA;
  } else if (0 == bit_width) {
  } else if (bit_offset < 0 ||
             bit_offset + bit_width > data_len * 8) {
    ret = common::OB_INVALID_DATA;
  } else {
    for (int i = 0; i < bit_width; ++i) {
      int64_t bit = bit_offset + i;
      uint64_t current = (static_cast<uint64_t>(data[bit >> 3]) >> (bit & 7)) & 1ULL;
      value |= current << i;
    }
  }
  return ret;
}

inline int ob_parquet_level_bit_width_(int64_t max_level)
{
  int width = 0;
  while (max_level > 0) {
    ++width;
    max_level >>= 1;
  }
  return width;
}

inline int ob_parquet_decode_rle_hybrid_(const uint8_t *data,
                                         int64_t len,
                                         int64_t num_values,
                                         int bit_width,
                                         std::vector<int64_t> &out,
                                         int64_t &consumed)
{
  int ret = common::OB_SUCCESS;
  out.clear();
  consumed = 0;
  int64_t pos = 0;
  if (num_values < 0 || bit_width < 0 || bit_width > 32) {
    ret = common::OB_INVALID_DATA;
  }
  while (OB_SUCC(ret) && static_cast<int64_t>(out.size()) < num_values) {
    uint64_t header = 0;
    if (!ob_parquet_read_varint_(data, len, pos, header)) {
      ret = common::OB_INVALID_DATA;
      break;
    } else if (0 == header) {
      ret = common::OB_INVALID_DATA;
      break;
    } else if ((header & 1ULL) != 0) {
      uint64_t groups = header >> 1;
      uint64_t bytes64 = groups * static_cast<uint64_t>(bit_width);
      if (bytes64 > static_cast<uint64_t>(len - pos)) {
        ret = common::OB_INVALID_DATA;
        break;
      }
      int64_t bytes = static_cast<int64_t>(bytes64);
      for (uint64_t g = 0; OB_SUCC(ret) && g < groups; ++g) {
        for (int j = 0; OB_SUCC(ret) && j < 8; ++j) {
          if (static_cast<int64_t>(out.size()) >= num_values) {
            break;
          }
          uint64_t value = 0;
          ret = ob_parquet_extract_bits_(data + pos,
                                         bytes,
                                         static_cast<int64_t>(g * 8 + j) * bit_width,
                                         bit_width,
                                         value);
          if (OB_SUCC(ret)) {
            out.push_back(static_cast<int64_t>(value));
          }
        }
      }
      if (OB_SUCC(ret)) {
        pos += bytes;
      }
    } else {
      uint64_t run_len = header >> 1;
      uint64_t value = 0;
      if (bit_width > 0) {
        if (pos + bit_width > len) {
          ret = common::OB_INVALID_DATA;
          break;
        }
        for (int i = 0; i < bit_width; ++i) {
          value |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
        }
        pos += bit_width;
      }
      if (OB_SUCC(ret)) {
        while (OB_SUCC(ret) &&
               static_cast<int64_t>(out.size()) < num_values &&
               run_len > 0) {
          out.push_back(static_cast<int64_t>(value));
          --run_len;
        }
      }
    }
  }
  if (OB_SUCC(ret) && static_cast<int64_t>(out.size()) != num_values) {
    ret = common::OB_INVALID_DATA;
  }
  consumed = pos;
  return ret;
}

inline uint32_t ob_parquet_read_le32_(const uint8_t *data)
{
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

inline uint64_t ob_parquet_read_le64_(const uint8_t *data)
{
  return static_cast<uint64_t>(data[0]) |
         (static_cast<uint64_t>(data[1]) << 8) |
         (static_cast<uint64_t>(data[2]) << 16) |
         (static_cast<uint64_t>(data[3]) << 24) |
         (static_cast<uint64_t>(data[4]) << 32) |
         (static_cast<uint64_t>(data[5]) << 40) |
         (static_cast<uint64_t>(data[6]) << 48) |
         (static_cast<uint64_t>(data[7]) << 56);
}

class ObFileParquetReader
{
public:
  ObFileParquetReader()
      : in_(),
        path_(),
        eof_(true),
        stat_valid_(false),
        stat_info_(),
        file_size_(0),
        num_rows_(0),
        row_count_(0),
        current_row_group_(-1),
        current_row_group_rows_(0),
        row_pos_(0),
        max_file_size_(0),
        max_rows_(0)
  {
    memset(&stat_info_, 0, sizeof(stat_info_));
  }

  ~ObFileParquetReader()
  {
    if (in_.is_open()) {
      in_.close();
    }
  }

  int open(const common::ObString &path, const ObFileSqlOptions &options)
  {
    int ret = common::OB_SUCCESS;
    path_ = std::string(path.ptr(), path.length());
    max_file_size_ = options.max_file_size_;
    max_rows_ = options.max_rows_;
    eof_ = false;
    stat_valid_ = false;
    file_size_ = 0;
    num_rows_ = 0;
    row_count_ = 0;
    current_row_group_ = -1;
    current_row_group_rows_ = 0;
    row_pos_ = 0;
    leaves_.clear();
    row_groups_.clear();
    decoded_columns_.clear();
    memset(&stat_info_, 0, sizeof(stat_info_));
    in_.clear();
    in_.open(path_, std::ios::binary | std::ios::ate);
    if (!in_.is_open()) {
      ret = common::OB_FILE_NOT_EXIST;
      eof_ = true;
    } else {
      file_size_ = static_cast<int64_t>(in_.tellg());
      if (file_size_ < 12) {
        ret = common::OB_INVALID_DATA;
        eof_ = true;
      } else if (max_file_size_ > 0 && file_size_ > max_file_size_) {
        ret = common::OB_SIZE_OVERFLOW;
        eof_ = true;
      } else if (0 == stat(path_.c_str(), &stat_info_)) {
        stat_valid_ = true;
      }
    }
    if (OB_SUCC(ret)) {
      char magic[4] = {0};
      in_.seekg(0, std::ios::beg);
      in_.read(magic, 4);
      if (!in_ || 0 != memcmp(magic, "PAR1", 4)) {
        ret = common::OB_INVALID_DATA;
      } else {
        in_.seekg(file_size_ - 8, std::ios::beg);
        uint8_t footer_len_buf[4] = {0};
        in_.read(reinterpret_cast<char *>(footer_len_buf), 4);
        uint32_t footer_len = static_cast<uint8_t>(footer_len_buf[0]) |
                              (static_cast<uint32_t>(footer_len_buf[1]) << 8) |
                              (static_cast<uint32_t>(footer_len_buf[2]) << 16) |
                              (static_cast<uint32_t>(footer_len_buf[3]) << 24);
        char end_magic[4] = {0};
        in_.read(end_magic, 4);
        if (!in_ || 0 != memcmp(end_magic, "PAR1", 4) ||
            static_cast<int64_t>(footer_len) > file_size_ - 12) {
          ret = common::OB_INVALID_DATA;
        } else {
          std::string footer(footer_len, '\0');
          in_.seekg(file_size_ - 8 - static_cast<int64_t>(footer_len), std::ios::beg);
          in_.read(&footer[0], static_cast<std::streamsize>(footer.size()));
          if (!in_ || in_.gcount() != static_cast<std::streamsize>(footer.size())) {
            ret = common::OB_IO_ERROR;
          } else if (OB_FAIL(parse_footer_(footer))) {
          }
        }
      }
    }
    if (OB_FAIL(ret)) {
      eof_ = true;
    }
    return ret;
  }

  int reset()
  {
    int ret = common::OB_SUCCESS;
    in_.clear();
    in_.seekg(0, std::ios::beg);
    eof_ = !in_.is_open();
    row_count_ = 0;
    current_row_group_ = -1;
    current_row_group_rows_ = 0;
    row_pos_ = 0;
    decoded_columns_.clear();
    return ret;
  }

  int next(std::vector<std::string> &values, std::vector<bool> &nulls)
  {
    int ret = common::OB_SUCCESS;
    values.clear();
    nulls.clear();
    if (eof_) {
      ret = common::OB_ITER_END;
    } else if (max_rows_ > 0 && row_count_ >= max_rows_) {
      ret = common::OB_SIZE_OVERFLOW;
    } else if (stat_valid_ && OB_FAIL(check_unchanged_())) {
    } else if (current_row_group_ < 0 || row_pos_ >= current_row_group_rows_) {
      if (OB_FAIL(load_next_row_group_())) {
        if (common::OB_ITER_END == ret) {
          eof_ = true;
        }
      }
    }
    if (OB_SUCC(ret)) {
      values.resize(leaves_.size());
      nulls.resize(leaves_.size());
      for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(leaves_.size()); ++i) {
        if (row_pos_ >= static_cast<int64_t>(decoded_columns_[i].values_.size()) ||
            row_pos_ >= static_cast<int64_t>(decoded_columns_[i].nulls_.size())) {
          ret = common::OB_INVALID_DATA;
        } else {
          values[i] = decoded_columns_[i].values_[row_pos_];
          nulls[i] = decoded_columns_[i].nulls_[row_pos_];
        }
      }
      if (OB_SUCC(ret)) {
        ++row_pos_;
        ++row_count_;
      }
    }
    return ret;
  }

private:
  struct PageHeaderInfo
  {
    PageHeaderInfo()
        : type_(-1),
          uncompressed_size_(0),
          compressed_size_(0),
          data_num_values_(0),
          data_encoding_(-1),
          data_def_encoding_(-1),
          data_rep_encoding_(-1),
          data_v2_num_values_(0),
          data_v2_num_nulls_(0),
          data_v2_num_rows_(0),
          data_v2_encoding_(-1),
          data_v2_def_len_(0),
          data_v2_rep_len_(0),
          data_v2_is_compressed_(true),
          dict_num_values_(0),
          dict_encoding_(-1),
          header_len_(0)
    {
    }
    int64_t type_;
    int64_t uncompressed_size_;
    int64_t compressed_size_;
    int64_t data_num_values_;
    int64_t data_encoding_;
    int64_t data_def_encoding_;
    int64_t data_rep_encoding_;
    int64_t data_v2_num_values_;
    int64_t data_v2_num_nulls_;
    int64_t data_v2_num_rows_;
    int64_t data_v2_encoding_;
    int64_t data_v2_def_len_;
    int64_t data_v2_rep_len_;
    bool data_v2_is_compressed_;
    int64_t dict_num_values_;
    int64_t dict_encoding_;
    int64_t header_len_;
  };

  struct ColumnChunkMeta
  {
    ColumnChunkMeta()
        : file_offset_(0),
          data_page_offset_(0),
          dictionary_page_offset_(0),
          num_values_(0),
          total_uncompressed_size_(0),
          total_compressed_size_(0),
          codec_(0)
    {
    }
    int64_t file_offset_;
    int64_t data_page_offset_;
    int64_t dictionary_page_offset_;
    int64_t num_values_;
    int64_t total_uncompressed_size_;
    int64_t total_compressed_size_;
    int64_t codec_;
  };

  struct RowGroupMeta
  {
    RowGroupMeta()
        : num_rows_(0)
    {
    }
    int64_t num_rows_;
    std::vector<ColumnChunkMeta> columns_;
  };

  struct LeafColumn
  {
    LeafColumn()
        : physical_type_(-1),
          type_length_(0),
          repetition_type_(0),
          converted_type_(-1),
          scale_(0),
          precision_(0),
          logical_type_(-1),
          logical_adjusted_to_utc_(false),
          logical_time_unit_(-1)
    {
    }
    std::string name_;
    int64_t physical_type_;
    int64_t type_length_;
    int64_t repetition_type_;
    int64_t converted_type_;
    int64_t scale_;
    int64_t precision_;
    int64_t logical_type_;
    bool logical_adjusted_to_utc_;
    int64_t logical_time_unit_;
  };

  struct DecodedColumn
  {
    std::vector<std::string> values_;
    std::vector<bool> nulls_;
  };

private:
  int parse_footer_(const std::string &footer)
  {
    int ret = common::OB_SUCCESS;
    leaves_.clear();
    row_groups_.clear();
    num_rows_ = 0;
    ObParquetMemorySource source(footer.data(), footer.size());
    ObParquetThriftReader<ObParquetMemorySource> reader(source);
    int16_t last_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if (2 == field_id && 9 == type) {
        uint8_t elem_type = 0;
        uint64_t schema_count = 0;
        if (OB_FAIL(reader.read_list_header(elem_type, schema_count))) {
        } else if (12 != elem_type) {
          ret = common::OB_INVALID_DATA;
        } else {
          for (uint64_t i = 0; OB_SUCC(ret) && i < schema_count; ++i) {
            if (OB_FAIL(parse_schema_element_(reader))) {
            }
          }
        }
      } else if (3 == field_id && (4 == type || 5 == type || 6 == type)) {
        if (OB_FAIL(reader.read_zigzag(num_rows_))) {
        }
      } else if (4 == field_id && 9 == type) {
        uint8_t elem_type = 0;
        uint64_t row_group_count = 0;
        if (OB_FAIL(reader.read_list_header(elem_type, row_group_count))) {
        } else if (12 != elem_type) {
          ret = common::OB_INVALID_DATA;
        } else {
          for (uint64_t i = 0; OB_SUCC(ret) && i < row_group_count; ++i) {
            if (OB_FAIL(parse_row_group_(reader))) {
            }
          }
        }
      } else {
        ret = reader.skip_value(type);
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; i < static_cast<int64_t>(leaves_.size()); ++i) {
        if (2 == leaves_[i].repetition_type_) {
          ret = common::OB_NOT_SUPPORTED;
          break;
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; i < static_cast<int64_t>(row_groups_.size()); ++i) {
        if (row_groups_[i].columns_.size() != leaves_.size()) {
          ret = common::OB_NOT_SUPPORTED;
          break;
        }
      }
    }
    return ret;
  }

  int parse_schema_element_(ObParquetThriftReader<ObParquetMemorySource> &reader)
  {
    int ret = common::OB_SUCCESS;
    LeafColumn leaf;
    int16_t last_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if ((1 == field_id || 2 == field_id || 3 == field_id || 5 == field_id ||
                  6 == field_id || 7 == field_id || 8 == field_id || 9 == field_id) &&
                 (3 == type || 4 == type || 5 == type || 6 == type)) {
        int64_t value = 0;
        if (OB_FAIL(reader.read_zigzag(value))) {
        } else if (1 == field_id) {
          leaf.physical_type_ = value;
        } else if (2 == field_id) {
          leaf.type_length_ = value;
        } else if (3 == field_id) {
          leaf.repetition_type_ = value;
        } else if (5 == field_id) {
          // num_children; ignored for leaf bookkeeping.
        } else if (6 == field_id) {
          leaf.converted_type_ = value;
        } else if (7 == field_id) {
          leaf.scale_ = value;
        } else if (8 == field_id) {
          leaf.precision_ = value;
        } else if (9 == field_id) {
          // field id; ignored.
        }
      } else if (4 == field_id && 8 == type) {
        if (OB_FAIL(reader.read_binary(leaf.name_))) {
        }
      } else if (10 == field_id && 12 == type) {
        if (OB_FAIL(parse_logical_type_(reader, leaf))) {
        }
      } else {
        ret = reader.skip_value(type);
      }
    }
    if (OB_SUCC(ret) && leaf.physical_type_ >= 0) {
      leaves_.push_back(leaf);
    }
    return ret;
  }

  int parse_logical_type_(ObParquetThriftReader<ObParquetMemorySource> &reader,
                          LeafColumn &leaf)
  {
    int ret = common::OB_SUCCESS;
    int16_t last_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if (5 == field_id && 12 == type) {
        leaf.logical_type_ = 5;
        if (OB_FAIL(parse_decimal_type_(reader, leaf))) {
        }
      } else if (6 == field_id && 12 == type) {
        leaf.logical_type_ = 6;
        ret = reader.skip_value(12);
      } else if (7 == field_id && 12 == type) {
        leaf.logical_type_ = 7;
        if (OB_FAIL(parse_temporal_type_(reader, leaf))) {
        }
      } else if (8 == field_id && 12 == type) {
        leaf.logical_type_ = 8;
        if (OB_FAIL(parse_temporal_type_(reader, leaf))) {
        }
      } else if (9 == field_id && 12 == type) {
        leaf.logical_type_ = 9;
        ret = parse_integer_type_(reader);
      } else if ((1 == field_id || 2 == field_id || 3 == field_id || 4 == field_id ||
                  10 == field_id || 11 == field_id || 12 == field_id || 13 == field_id ||
                  14 == field_id) && 12 == type) {
        leaf.logical_type_ = field_id;
        ret = reader.skip_value(12);
      } else {
        ret = reader.skip_value(type);
      }
    }
    return ret;
  }

  int parse_decimal_type_(ObParquetThriftReader<ObParquetMemorySource> &reader,
                          LeafColumn &leaf)
  {
    int ret = common::OB_SUCCESS;
    int16_t last_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if ((1 == field_id || 2 == field_id) &&
                 (3 == type || 4 == type || 5 == type || 6 == type)) {
        int64_t value = 0;
        if (OB_FAIL(reader.read_zigzag(value))) {
        } else if (1 == field_id) {
          leaf.scale_ = value;
        } else {
          leaf.precision_ = value;
        }
      } else {
        ret = reader.skip_value(type);
      }
    }
    return ret;
  }

  int parse_temporal_type_(ObParquetThriftReader<ObParquetMemorySource> &reader,
                           LeafColumn &leaf)
  {
    int ret = common::OB_SUCCESS;
    int16_t last_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if (1 == field_id && (1 == type || 2 == type)) {
        leaf.logical_adjusted_to_utc_ = (1 == type);
      } else if (2 == field_id && 12 == type) {
        if (OB_FAIL(parse_time_unit_(reader, leaf.logical_time_unit_))) {
        }
      } else {
        ret = reader.skip_value(type);
      }
    }
    return ret;
  }

  int parse_time_unit_(ObParquetThriftReader<ObParquetMemorySource> &reader,
                       int64_t &unit)
  {
    int ret = common::OB_SUCCESS;
    int16_t last_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if ((1 == field_id || 2 == field_id || 3 == field_id) && 12 == type) {
        unit = field_id - 1;
        ret = reader.skip_value(12);
      } else {
        ret = reader.skip_value(type);
      }
    }
    return ret;
  }

  int parse_integer_type_(ObParquetThriftReader<ObParquetMemorySource> &reader)
  {
    int ret = common::OB_SUCCESS;
    int16_t last_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else {
        ret = reader.skip_value(type);
      }
    }
    return ret;
  }

  int parse_row_group_(ObParquetThriftReader<ObParquetMemorySource> &reader)
  {
    int ret = common::OB_SUCCESS;
    RowGroupMeta row_group;
    int16_t last_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if (1 == field_id && 9 == type) {
        uint8_t elem_type = 0;
        uint64_t column_count = 0;
        if (OB_FAIL(reader.read_list_header(elem_type, column_count))) {
        } else if (12 != elem_type) {
          ret = common::OB_INVALID_DATA;
        } else {
          for (uint64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
            ColumnChunkMeta chunk;
            if (OB_FAIL(parse_column_chunk_(reader, chunk))) {
            } else {
              row_group.columns_.push_back(chunk);
            }
          }
        }
      } else if (3 == field_id && (4 == type || 5 == type || 6 == type)) {
        if (OB_FAIL(reader.read_zigzag(row_group.num_rows_))) {
        }
      } else {
        ret = reader.skip_value(type);
      }
    }
    if (OB_SUCC(ret)) {
      row_groups_.push_back(row_group);
    }
    return ret;
  }

  int parse_column_chunk_(ObParquetThriftReader<ObParquetMemorySource> &reader,
                          ColumnChunkMeta &chunk)
  {
    int ret = common::OB_SUCCESS;
    int16_t last_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if (2 == field_id && (4 == type || 5 == type || 6 == type)) {
        if (OB_FAIL(reader.read_zigzag(chunk.file_offset_))) {
        }
      } else if (3 == field_id && 12 == type) {
        if (OB_FAIL(parse_column_metadata_(reader, chunk))) {
        }
      } else {
        ret = reader.skip_value(type);
      }
    }
    return ret;
  }

  int parse_column_metadata_(ObParquetThriftReader<ObParquetMemorySource> &reader,
                             ColumnChunkMeta &chunk)
  {
    int ret = common::OB_SUCCESS;
    int16_t last_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if ((4 == field_id || 5 == field_id || 6 == field_id || 7 == field_id ||
                  9 == field_id || 11 == field_id) &&
                 (3 == type || 4 == type || 5 == type || 6 == type)) {
        int64_t value = 0;
        if (OB_FAIL(reader.read_zigzag(value))) {
        } else if (4 == field_id) {
          chunk.codec_ = value;
        } else if (5 == field_id) {
          chunk.num_values_ = value;
        } else if (6 == field_id) {
          chunk.total_uncompressed_size_ = value;
        } else if (7 == field_id) {
          chunk.total_compressed_size_ = value;
        } else if (9 == field_id) {
          chunk.data_page_offset_ = value;
        } else if (11 == field_id) {
          chunk.dictionary_page_offset_ = value;
        }
      } else {
        ret = reader.skip_value(type);
      }
    }
    return ret;
  }

  int load_next_row_group_()
  {
    int ret = common::OB_SUCCESS;
    ++current_row_group_;
    if (current_row_group_ >= static_cast<int64_t>(row_groups_.size())) {
      ret = common::OB_ITER_END;
      eof_ = true;
    } else {
      const RowGroupMeta &row_group = row_groups_[current_row_group_];
      decoded_columns_.clear();
      decoded_columns_.resize(leaves_.size());
      for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(leaves_.size()); ++i) {
        if (OB_FAIL(decode_column_chunk_(i, row_group.columns_[i], decoded_columns_[i]))) {
        }
      }
      if (OB_SUCC(ret)) {
        current_row_group_rows_ = row_group.num_rows_;
        row_pos_ = 0;
      } else {
        decoded_columns_.clear();
      }
    }
    return ret;
  }

  int decode_column_chunk_(int64_t column_idx,
                           const ColumnChunkMeta &chunk,
                           DecodedColumn &out)
  {
    int ret = common::OB_SUCCESS;
    out.values_.clear();
    out.nulls_.clear();
    std::vector<std::string> dict;
    if (chunk.dictionary_page_offset_ > 0) {
      if (OB_FAIL(read_dictionary_(leaves_[column_idx], chunk, dict))) {
      }
    }
    if (OB_SUCC(ret)) {
      int64_t values_left = chunk.num_values_;
      int64_t page_offset = chunk.data_page_offset_;
      if (page_offset <= 0 || page_offset >= file_size_) {
        ret = common::OB_INVALID_DATA;
      }
      while (OB_SUCC(ret) && values_left > 0) {
        PageHeaderInfo header;
        if (OB_FAIL(read_page_header_(page_offset, header))) {
          break;
        }
        std::vector<char> raw(static_cast<size_t>(header.compressed_size_));
        if (OB_FAIL(read_bytes_at_(page_offset + header.header_len_,
                                   raw.data(),
                                   header.compressed_size_))) {
          break;
        }
        if (0 == header.type_) {
          if (header.data_num_values_ <= 0 || header.data_num_values_ > values_left) {
            ret = common::OB_INVALID_DATA;
            break;
          }
          std::vector<char> payload;
          if (OB_FAIL(decompress_page_(raw,
                                       header.uncompressed_size_,
                                       chunk.codec_,
                                       payload))) {
            break;
          }
          if (OB_FAIL(decode_data_page_v1_(leaves_[column_idx],
                                           payload,
                                           header,
                                           dict,
                                           out))) {
            break;
          }
          values_left -= header.data_num_values_;
        } else if (3 == header.type_) {
          if (header.data_v2_num_values_ <= 0 ||
              header.data_v2_num_values_ > values_left) {
            ret = common::OB_INVALID_DATA;
            break;
          }
          if (OB_FAIL(decode_data_page_v2_(leaves_[column_idx],
                                           raw,
                                           header,
                                           chunk.codec_,
                                           dict,
                                           out))) {
            break;
          }
          values_left -= header.data_v2_num_values_;
        } else if (1 == header.type_) {
          // Index page; skip.
        } else {
          ret = common::OB_INVALID_DATA;
          break;
        }
        page_offset += header.header_len_ + header.compressed_size_;
      }
    }
    if (OB_SUCC(ret) &&
        (static_cast<int64_t>(out.values_.size()) != chunk.num_values_ ||
         static_cast<int64_t>(out.nulls_.size()) != chunk.num_values_)) {
      ret = common::OB_INVALID_DATA;
    }
    return ret;
  }

  int read_dictionary_(const LeafColumn &column,
                       const ColumnChunkMeta &chunk,
                       std::vector<std::string> &dict)
  {
    int ret = common::OB_SUCCESS;
    dict.clear();
    PageHeaderInfo header;
    if (OB_FAIL(read_page_header_(chunk.dictionary_page_offset_, header))) {
    } else if (2 != header.type_) {
      ret = common::OB_INVALID_DATA;
    } else {
      std::vector<char> raw(static_cast<size_t>(header.compressed_size_));
      if (OB_FAIL(read_bytes_at_(chunk.dictionary_page_offset_ + header.header_len_,
                                 raw.data(),
                                 header.compressed_size_))) {
      } else {
        std::vector<char> payload;
        if (OB_FAIL(decompress_page_(raw,
                                     header.uncompressed_size_,
                                     chunk.codec_,
                                     payload))) {
        } else if (0 != header.dict_encoding_) {
          ret = common::OB_NOT_SUPPORTED;
        } else if (0 == column.physical_type_) {
          if (OB_FAIL(decode_plain_booleans_(
                  reinterpret_cast<const uint8_t *>(payload.data()),
                  payload.size(),
                  header.dict_num_values_,
                  dict))) {
          }
        } else if (OB_FAIL(decode_plain_values_(
                       column,
                       reinterpret_cast<const uint8_t *>(payload.data()),
                       payload.size(),
                       header.dict_num_values_,
                       dict))) {
        }
      }
    }
    return ret;
  }

  int read_page_header_(int64_t page_offset, PageHeaderInfo &header)
  {
    int ret = common::OB_SUCCESS;
    header = PageHeaderInfo();
    if (page_offset < 0 || page_offset >= file_size_) {
      ret = common::OB_INVALID_DATA;
    } else {
      in_.clear();
      in_.seekg(page_offset, std::ios::beg);
      if (!in_) {
        ret = common::OB_IO_ERROR;
      } else {
        ObParquetFileSource source(in_, file_size_ - page_offset);
        ObParquetThriftReader<ObParquetFileSource> reader(source);
        int16_t last_id = 0;
        while (OB_SUCC(ret)) {
          uint8_t type = 0;
          int16_t field_id = 0;
          if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
          } else if (0 == type) {
            break;
          } else if ((1 == field_id || 2 == field_id || 3 == field_id || 4 == field_id) &&
                     (3 == type || 4 == type || 5 == type || 6 == type)) {
            int64_t value = 0;
            if (OB_FAIL(reader.read_zigzag(value))) {
            } else if (1 == field_id) {
              header.type_ = value;
            } else if (2 == field_id) {
              header.uncompressed_size_ = value;
            } else if (3 == field_id) {
              header.compressed_size_ = value;
            }
          } else if (5 == field_id && 12 == type) {
            if (OB_FAIL(parse_page_data_header_(reader, header))) {
            }
          } else if (6 == field_id && 12 == type) {
            ret = reader.skip_value(12);
          } else if (7 == field_id && 12 == type) {
            if (OB_FAIL(parse_page_dictionary_header_(reader, header))) {
            }
          } else if (8 == field_id && 12 == type) {
            if (OB_FAIL(parse_page_data_header_v2_(reader, header))) {
            }
          } else {
            ret = reader.skip_value(type);
          }
        }
        if (OB_SUCC(ret)) {
          header.header_len_ = reader.position();
          if (header.compressed_size_ <= 0) {
            header.compressed_size_ = header.uncompressed_size_;
          }
          if (header.uncompressed_size_ <= 0) {
            header.uncompressed_size_ = header.compressed_size_;
          }
          if (header.type_ < 0 || header.type_ > 3 ||
              header.compressed_size_ <= 0 ||
              page_offset + header.header_len_ + header.compressed_size_ > file_size_) {
            ret = common::OB_INVALID_DATA;
          }
        }
      }
    }
    return ret;
  }

  int parse_page_data_header_(ObParquetThriftReader<ObParquetFileSource> &reader,
                              PageHeaderInfo &header)
  {
    int ret = common::OB_SUCCESS;
    int16_t last_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if ((1 == field_id || 2 == field_id || 3 == field_id || 4 == field_id) &&
                 (3 == type || 4 == type || 5 == type || 6 == type)) {
        int64_t value = 0;
        if (OB_FAIL(reader.read_zigzag(value))) {
        } else if (1 == field_id) {
          header.data_num_values_ = value;
        } else if (2 == field_id) {
          header.data_encoding_ = value;
        } else if (3 == field_id) {
          header.data_def_encoding_ = value;
        } else if (4 == field_id) {
          header.data_rep_encoding_ = value;
        }
      } else {
        ret = reader.skip_value(type);
      }
    }
    return ret;
  }

  int parse_page_data_header_v2_(ObParquetThriftReader<ObParquetFileSource> &reader,
                                 PageHeaderInfo &header)
  {
    int ret = common::OB_SUCCESS;
    int16_t last_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if ((1 == field_id || 2 == field_id || 3 == field_id || 4 == field_id ||
                  5 == field_id || 6 == field_id) &&
                 (3 == type || 4 == type || 5 == type || 6 == type)) {
        int64_t value = 0;
        if (OB_FAIL(reader.read_zigzag(value))) {
        } else if (1 == field_id) {
          header.data_v2_num_values_ = value;
        } else if (2 == field_id) {
          header.data_v2_num_nulls_ = value;
        } else if (3 == field_id) {
          header.data_v2_num_rows_ = value;
        } else if (4 == field_id) {
          header.data_v2_encoding_ = value;
        } else if (5 == field_id) {
          header.data_v2_def_len_ = value;
        } else if (6 == field_id) {
          header.data_v2_rep_len_ = value;
        }
      } else if (7 == field_id && (1 == type || 2 == type)) {
        header.data_v2_is_compressed_ = (1 == type);
      } else {
        ret = reader.skip_value(type);
      }
    }
    return ret;
  }

  int parse_page_dictionary_header_(ObParquetThriftReader<ObParquetFileSource> &reader,
                                    PageHeaderInfo &header)
  {
    int ret = common::OB_SUCCESS;
    int16_t last_id = 0;
    while (OB_SUCC(ret)) {
      uint8_t type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(type, field_id, last_id))) {
      } else if (0 == type) {
        break;
      } else if ((1 == field_id || 2 == field_id) &&
                 (3 == type || 4 == type || 5 == type || 6 == type)) {
        int64_t value = 0;
        if (OB_FAIL(reader.read_zigzag(value))) {
        } else if (1 == field_id) {
          header.dict_num_values_ = value;
        } else if (2 == field_id) {
          header.dict_encoding_ = value;
        }
      } else if (3 == field_id && (1 == type || 2 == type)) {
        // is_sorted; ignored.
      } else {
        ret = reader.skip_value(type);
      }
    }
    return ret;
  }

  int decompress_page_(const std::vector<char> &raw,
                       int64_t uncompressed_size,
                       int64_t codec,
                       std::vector<char> &out)
  {
    int ret = common::OB_SUCCESS;
    out.clear();
    if (0 == codec) {
      out = raw;
      if (uncompressed_size > 0 &&
          static_cast<int64_t>(out.size()) != uncompressed_size) {
        ret = common::OB_INVALID_DATA;
      }
    } else if (1 == codec) {
      if (OB_FAIL(snappy_decompress(raw.data(), raw.size(), out))) {
      } else if (uncompressed_size > 0 &&
                 static_cast<int64_t>(out.size()) != uncompressed_size) {
        ret = common::OB_INVALID_DATA;
      }
    } else {
      ret = common::OB_NOT_SUPPORTED;
    }
    return ret;
  }

  int decode_data_page_v1_(const LeafColumn &column,
                           const std::vector<char> &payload,
                           const PageHeaderInfo &header,
                           const std::vector<std::string> &dict,
                           DecodedColumn &out)
  {
    int ret = common::OB_SUCCESS;
    const uint8_t *data = reinterpret_cast<const uint8_t *>(payload.data());
    int64_t len = static_cast<int64_t>(payload.size());
    int64_t max_definition_level = column.repetition_type_ == 1 ? 1 : 0;
    std::vector<int64_t> levels;
    int64_t values_offset = 0;
    if (max_definition_level > 0) {
      int bit_width = ob_parquet_level_bit_width_(max_definition_level);
      if (OB_FAIL(ob_parquet_decode_rle_hybrid_(data,
                                                len,
                                                header.data_num_values_,
                                                bit_width,
                                                levels,
                                                values_offset))) {
      }
    } else {
      levels.assign(header.data_num_values_, 0);
    }
    if (OB_SUCC(ret)) {
      std::vector<char> values_payload(data + values_offset, data + len);
      ret = decode_values_(column,
                           values_payload,
                           header.data_num_values_,
                           header.data_encoding_,
                           levels,
                           dict,
                           out);
    }
    return ret;
  }

  int decode_data_page_v2_(const LeafColumn &column,
                           const std::vector<char> &raw,
                           const PageHeaderInfo &header,
                           int64_t codec,
                           const std::vector<std::string> &dict,
                           DecodedColumn &out)
  {
    int ret = common::OB_SUCCESS;
    const uint8_t *data = reinterpret_cast<const uint8_t *>(raw.data());
    int64_t raw_len = static_cast<int64_t>(raw.size());
    int64_t rep_len = header.data_v2_rep_len_;
    int64_t def_len = header.data_v2_def_len_;
    if (rep_len < 0 || def_len < 0 || rep_len + def_len > raw_len) {
      ret = common::OB_INVALID_DATA;
    } else {
      int64_t max_definition_level = column.repetition_type_ == 1 ? 1 : 0;
      std::vector<int64_t> levels;
      if (max_definition_level > 0) {
        if (def_len <= 0) {
          ret = common::OB_INVALID_DATA;
        } else {
          int bit_width = ob_parquet_level_bit_width_(max_definition_level);
          int64_t consumed = 0;
          if (OB_FAIL(ob_parquet_decode_rle_hybrid_(data + rep_len,
                                                    def_len,
                                                    header.data_v2_num_values_,
                                                    bit_width,
                                                    levels,
                                                    consumed))) {
          } else if (consumed != def_len) {
            ret = common::OB_INVALID_DATA;
          }
        }
      } else {
        if (def_len != 0) {
          ret = common::OB_INVALID_DATA;
        } else {
          levels.assign(header.data_v2_num_values_, 0);
        }
      }
      if (OB_SUCC(ret)) {
        int64_t values_len = raw_len - rep_len - def_len;
        std::vector<char> values_raw(data + rep_len + def_len,
                                     data + rep_len + def_len + values_len);
        std::vector<char> values_payload;
        int64_t uncompressed_values_size = header.uncompressed_size_ > 0
                                               ? header.uncompressed_size_ - rep_len - def_len
                                               : 0;
        if (uncompressed_values_size < 0) {
          ret = common::OB_INVALID_DATA;
        } else if (header.data_v2_is_compressed_) {
          if (OB_FAIL(decompress_page_(values_raw,
                                       uncompressed_values_size,
                                       codec,
                                       values_payload))) {
          }
        } else {
          values_payload = values_raw;
        }
        if (OB_SUCC(ret)) {
          ret = decode_values_(column,
                               values_payload,
                               header.data_v2_num_values_,
                               header.data_v2_encoding_,
                               levels,
                               dict,
                               out);
        }
      }
    }
    return ret;
  }

  int decode_values_(const LeafColumn &column,
                     const std::vector<char> &payload,
                     int64_t num_values,
                     int64_t encoding,
                     const std::vector<int64_t> &levels,
                     const std::vector<std::string> &dict,
                     DecodedColumn &out)
  {
    int ret = common::OB_SUCCESS;
    const uint8_t *data = reinterpret_cast<const uint8_t *>(payload.data());
    int64_t len = static_cast<int64_t>(payload.size());
    int64_t max_definition_level = column.repetition_type_ == 1 ? 1 : 0;
    std::vector<std::string> values;
    if (0 == encoding) {
      if (0 == column.physical_type_) {
        if (OB_FAIL(decode_plain_booleans_(data, len, num_values, values))) {
        }
      } else if (OB_FAIL(decode_plain_values_(column, data, len, num_values, values))) {
      }
      if (OB_SUCC(ret)) {
        for (int64_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
          if (levels[i] >= max_definition_level) {
            out.values_.push_back(values[i]);
            out.nulls_.push_back(false);
          } else {
            out.values_.push_back("");
            out.nulls_.push_back(true);
          }
        }
      }
    } else if (2 == encoding || 8 == encoding) {
      if (len < 1) {
        ret = common::OB_INVALID_DATA;
      } else {
        int bit_width = static_cast<int>(data[0]);
        std::vector<int64_t> indices;
        int64_t consumed = 0;
        if (OB_FAIL(ob_parquet_decode_rle_hybrid_(data + 1,
                                                  len - 1,
                                                  num_values,
                                                  bit_width,
                                                  indices,
                                                  consumed))) {
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
            if (levels[i] >= max_definition_level) {
              if (indices[i] < 0 ||
                  indices[i] >= static_cast<int64_t>(dict.size())) {
                ret = common::OB_INVALID_DATA;
              } else {
                out.values_.push_back(dict[indices[i]]);
                out.nulls_.push_back(false);
              }
            } else {
              out.values_.push_back("");
              out.nulls_.push_back(true);
            }
          }
        }
      }
    } else {
      ret = common::OB_NOT_SUPPORTED;
    }
    return ret;
  }

  int decode_plain_values_(const LeafColumn &column,
                           const uint8_t *data,
                           int64_t len,
                           int64_t num_values,
                           std::vector<std::string> &out)
  {
    int ret = common::OB_SUCCESS;
    out.clear();
    out.reserve(static_cast<size_t>(num_values));
    int64_t pos = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
      std::string value;
      if (1 == column.physical_type_) {
        if (pos + 4 > len) {
          ret = common::OB_INVALID_DATA;
        } else {
          int32_t raw = static_cast<int32_t>(ob_parquet_read_le32_(data + pos));
          if (OB_FAIL(format_int32_value_(column, raw, value))) {
          }
          pos += 4;
        }
      } else if (2 == column.physical_type_) {
        if (pos + 8 > len) {
          ret = common::OB_INVALID_DATA;
        } else {
          int64_t raw = static_cast<int64_t>(ob_parquet_read_le64_(data + pos));
          if (OB_FAIL(format_int64_value_(column, raw, value))) {
          }
          pos += 8;
        }
      } else if (3 == column.physical_type_) {
        if (pos + 12 > len) {
          ret = common::OB_INVALID_DATA;
        } else {
          if (OB_FAIL(format_int96_value_(data + pos, value))) {
          }
          pos += 12;
        }
      } else if (4 == column.physical_type_) {
        if (pos + 4 > len) {
          ret = common::OB_INVALID_DATA;
        } else {
          uint32_t bits = ob_parquet_read_le32_(data + pos);
          float raw = 0.0f;
          memcpy(&raw, &bits, sizeof(raw));
          if (OB_FAIL(format_float_value_(raw, value))) {
          }
          pos += 4;
        }
      } else if (5 == column.physical_type_) {
        if (pos + 8 > len) {
          ret = common::OB_INVALID_DATA;
        } else {
          uint64_t bits = ob_parquet_read_le64_(data + pos);
          double raw = 0.0;
          memcpy(&raw, &bits, sizeof(raw));
          if (OB_FAIL(format_double_value_(raw, value))) {
          }
          pos += 8;
        }
      } else if (6 == column.physical_type_) {
        if (pos + 4 > len) {
          ret = common::OB_INVALID_DATA;
        } else {
          uint32_t value_len = ob_parquet_read_le32_(data + pos);
          pos += 4;
          if (static_cast<int64_t>(value_len) > len - pos) {
            ret = common::OB_INVALID_DATA;
          } else {
            if (OB_FAIL(format_byte_array_value_(column,
                                                data + pos,
                                                value_len,
                                                value))) {
            }
            pos += value_len;
          }
        }
      } else if (7 == column.physical_type_) {
        if (column.type_length_ <= 0 ||
            pos + column.type_length_ > len) {
          ret = common::OB_INVALID_DATA;
        } else {
          if (OB_FAIL(format_byte_array_value_(column,
                                              data + pos,
                                              column.type_length_,
                                              value))) {
          }
          pos += column.type_length_;
        }
      } else {
        ret = common::OB_NOT_SUPPORTED;
      }
      if (OB_SUCC(ret)) {
        out.push_back(value);
      }
    }
    return ret;
  }

  int decode_plain_booleans_(const uint8_t *data,
                             int64_t len,
                             int64_t num_values,
                             std::vector<std::string> &out)
  {
    int ret = common::OB_SUCCESS;
    out.clear();
    out.reserve(static_cast<size_t>(num_values));
    int64_t byte_count = (num_values + 7) / 8;
    if (byte_count > len) {
      ret = common::OB_INVALID_DATA;
    } else {
      for (int64_t i = 0; i < num_values; ++i) {
        out.push_back(((data[i / 8] >> (i % 8)) & 1) != 0 ? "true" : "false");
      }
    }
    return ret;
  }

  int read_bytes_at_(int64_t offset, char *buf, int64_t len)
  {
    int ret = common::OB_SUCCESS;
    if (offset < 0 || len < 0 || offset + len > file_size_) {
      ret = common::OB_INVALID_DATA;
    } else {
      in_.clear();
      in_.seekg(offset, std::ios::beg);
      if (!in_) {
        ret = common::OB_IO_ERROR;
      } else {
        in_.read(buf, static_cast<std::streamsize>(len));
        if (!in_ || in_.gcount() != static_cast<std::streamsize>(len)) {
          ret = common::OB_IO_ERROR;
        }
      }
    }
    return ret;
  }

  int check_unchanged_()
  {
    int ret = common::OB_SUCCESS;
    struct stat current;
    memset(&current, 0, sizeof(current));
    if (0 != stat(path_.c_str(), &current)) {
      ret = common::OB_FILE_NOT_EXIST;
    } else if (current.st_size != stat_info_.st_size ||
               current.st_mtime != stat_info_.st_mtime) {
      ret = common::OB_IO_ERROR;
    }
    return ret;
  }

  bool is_decimal_(const LeafColumn &column) const
  {
    return 5 == column.converted_type_ || 5 == column.logical_type_;
  }

  bool is_date_(const LeafColumn &column) const
  {
    return 6 == column.converted_type_ || 6 == column.logical_type_;
  }

  bool is_timestamp_(const LeafColumn &column) const
  {
    return 9 == column.converted_type_ || 10 == column.converted_type_ ||
           8 == column.logical_type_;
  }

  int64_t timestamp_unit_(const LeafColumn &column) const
  {
    if (9 == column.converted_type_) {
      return 0;
    }
    if (10 == column.converted_type_) {
      return 1;
    }
    if (column.logical_time_unit_ >= 0 && column.logical_time_unit_ <= 2) {
      return column.logical_time_unit_;
    }
    return 1;
  }

  int format_int32_value_(const LeafColumn &column,
                          int32_t value,
                          std::string &out) const
  {
    int ret = common::OB_SUCCESS;
    if (is_date_(column)) {
      ret = format_date_(value, out);
    } else {
      out = std::to_string(value);
    }
    return ret;
  }

  int format_int64_value_(const LeafColumn &column,
                          int64_t value,
                          std::string &out) const
  {
    int ret = common::OB_SUCCESS;
    if (is_timestamp_(column)) {
      ret = format_timestamp_(value, timestamp_unit_(column), out);
    } else {
      out = std::to_string(value);
    }
    return ret;
  }

  int format_int96_value_(const uint8_t *data, std::string &out) const
  {
    int ret = common::OB_SUCCESS;
    uint64_t nanos_of_day = ob_parquet_read_le64_(data);
    uint32_t julian_day = ob_parquet_read_le32_(data + 8);
    if (nanos_of_day >= 86400ULL * 1000000000ULL) {
      ret = common::OB_INVALID_DATA;
    } else {
      int64_t days = static_cast<int64_t>(julian_day) - 2440588;
      int64_t seconds_of_day = static_cast<int64_t>(nanos_of_day / 1000000000ULL);
      std::string date;
      if (OB_FAIL(format_date_(days, date))) {
      } else {
        out = date + " " + format_time_of_day_(seconds_of_day);
      }
    }
    return ret;
  }

  int format_timestamp_(int64_t value, int64_t unit, std::string &out) const
  {
    int ret = common::OB_SUCCESS;
    int64_t scale = 1;
    if (0 == unit) {
      scale = 1000;
    } else if (1 == unit) {
      scale = 1000000;
    } else {
      scale = 1000000000;
    }
    int64_t seconds = value / scale;
    int64_t remainder = value % scale;
    if (remainder < 0) {
      --seconds;
      remainder += scale;
    }
    int64_t days = seconds / 86400;
    int64_t seconds_of_day = seconds % 86400;
    if (seconds_of_day < 0) {
      --days;
      seconds_of_day += 86400;
    }
    std::string date;
    if (OB_FAIL(format_date_(days, date))) {
    } else {
      out = date + " " + format_time_of_day_(seconds_of_day);
    }
    return ret;
  }

  int format_float_value_(float value, std::string &out) const
  {
    char buf[64] = {0};
    int len = snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(value));
    out.assign(buf, static_cast<size_t>(len));
    return common::OB_SUCCESS;
  }

  int format_double_value_(double value, std::string &out) const
  {
    char buf[128] = {0};
    int len = snprintf(buf, sizeof(buf), "%.17g", value);
    out.assign(buf, static_cast<size_t>(len));
    return common::OB_SUCCESS;
  }

  int format_byte_array_value_(const LeafColumn &column,
                               const uint8_t *data,
                               int64_t len,
                               std::string &out) const
  {
    int ret = common::OB_SUCCESS;
    if (is_decimal_(column)) {
      ret = format_decimal_(data, len, column.scale_, out);
    } else {
      out.assign(reinterpret_cast<const char *>(data), static_cast<size_t>(len));
    }
    return ret;
  }

  int format_decimal_(const uint8_t *data,
                      int64_t len,
                      int64_t scale,
                      std::string &out) const
  {
    int ret = common::OB_SUCCESS;
    out.clear();
    if (len <= 0 || scale < 0) {
      ret = common::OB_INVALID_DATA;
    } else {
      bool negative = (data[0] & 0x80) != 0;
      std::vector<uint8_t> magnitude(data, data + len);
      if (negative) {
        for (int64_t i = 0; i < len; ++i) {
          magnitude[i] = static_cast<uint8_t>(~magnitude[i]);
        }
        int carry = 1;
        for (int64_t i = len - 1; i >= 0 && carry != 0; --i) {
          int value = static_cast<int>(magnitude[i]) + carry;
          magnitude[i] = static_cast<uint8_t>(value & 0xff);
          carry = value >> 8;
        }
      }
      std::string digits = "0";
      for (int64_t i = 0; i < len; ++i) {
        digits = decimal_mul_small_(digits, 256);
        digits = decimal_add_small_(digits, magnitude[i]);
      }
      while (digits.size() > 1 && '0' == digits[0]) {
        digits.erase(digits.begin());
      }
      if (0 == scale) {
        out = digits;
      } else if (scale >= static_cast<int64_t>(digits.size())) {
        out = "0.";
        out.append(static_cast<size_t>(scale - digits.size()), '0');
        out.append(digits);
      } else {
        size_t split = digits.size() - static_cast<size_t>(scale);
        std::string fraction = digits.substr(split);
        while (!fraction.empty() && '0' == fraction.back()) {
          fraction.pop_back();
        }
        out = digits.substr(0, split);
        if (!fraction.empty()) {
          out += ".";
          out += fraction;
        }
      }
      if (negative && out != "0") {
        out = "-" + out;
      }
    }
    return ret;
  }

  std::string decimal_mul_small_(const std::string &digits, int multiplier) const
  {
    std::string result = digits;
    int carry = 0;
    for (int i = static_cast<int>(result.size()) - 1; i >= 0; --i) {
      int value = (result[i] - '0') * multiplier + carry;
      result[i] = static_cast<char>('0' + value % 10);
      carry = value / 10;
    }
    while (carry > 0) {
      result.insert(result.begin(), static_cast<char>('0' + carry % 10));
      carry /= 10;
    }
    return result;
  }

  std::string decimal_add_small_(const std::string &digits, int addend) const
  {
    std::string result = digits;
    int carry = addend;
    for (int i = static_cast<int>(result.size()) - 1; i >= 0 && carry > 0; --i) {
      int value = (result[i] - '0') + carry;
      result[i] = static_cast<char>('0' + value % 10);
      carry = value / 10;
    }
    while (carry > 0) {
      result.insert(result.begin(), static_cast<char>('0' + carry % 10));
      carry /= 10;
    }
    return result;
  }

  int format_date_(int64_t days, std::string &out) const
  {
    int64_t year = 0;
    int month = 0;
    int day = 0;
    days_to_civil_(days, year, month, day);
    char buf[64] = {0};
    int len = snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                       static_cast<int>(year), month, day);
    out.assign(buf, static_cast<size_t>(len));
    return common::OB_SUCCESS;
  }

  void days_to_civil_(int64_t z, int64_t &year, int &month, int &day) const
  {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = static_cast<uint64_t>(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = static_cast<int64_t>(yoe) + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp = (5 * doy + 2) / 153;
    uint64_t d = doy - (153 * mp + 2) / 5 + 1;
    uint64_t m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    year = y;
    month = static_cast<int>(m);
    day = static_cast<int>(d);
  }

  std::string format_time_of_day_(int64_t seconds_of_day) const
  {
    int64_t hours = seconds_of_day / 3600;
    int64_t minutes = (seconds_of_day % 3600) / 60;
    int64_t seconds = seconds_of_day % 60;
    char buf[16] = {0};
    snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld",
             static_cast<long long>(hours),
             static_cast<long long>(minutes),
             static_cast<long long>(seconds));
    return std::string(buf);
  }

private:
  std::ifstream in_;
  std::string path_;
  bool eof_;
  bool stat_valid_;
  struct stat stat_info_;
  int64_t file_size_;
  int64_t num_rows_;
  int64_t row_count_;
  std::vector<LeafColumn> leaves_;
  std::vector<RowGroupMeta> row_groups_;
  int64_t current_row_group_;
  int64_t current_row_group_rows_;
  int64_t row_pos_;
  std::vector<DecodedColumn> decoded_columns_;
  int64_t max_file_size_;
  int64_t max_rows_;
};

} // namespace sql
} // namespace oceanbase
