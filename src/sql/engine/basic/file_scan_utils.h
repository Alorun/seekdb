/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <string>
#include <vector>
#include <sys/stat.h>
#include "common/object/ob_object.h"
#include "sql/parser/parse_node.h"

namespace oceanbase
{
namespace sql
{
class ObSQLSessionInfo;
class ObRawExpr;
class ObExecContext;
namespace file_sql
{
constexpr size_t MAX_RECORD = 8 * 1024 * 1024;
constexpr size_t MAX_COLUMNS = 1024;
constexpr size_t MAX_SCHEMA = 1024 * 1024;
enum class Type
{
  NIL,
  INT,
  DOUBLE,
  BOOL,
  STRING,
  DATE,
  DATETIME
};
struct Column
{
  std::string name;
  Type type = Type::NIL;
  bool nullable = false;
  bool wide_int = false;
  int scale = 0;
  uint64_t seen = 0;
};
struct Options
{
  int format = 1; // CSV, NDJSON, Parquet, glob
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string nullstr;
};
struct Identity
{
  uint64_t dev = 0, ino = 0, size = 0;
  int64_t mtime = 0, mtime_ns = 0, ctime = 0, ctime_ns = 0;
  void assign(const struct stat &s);
  bool matches(const struct stat &s) const;
};
struct Descriptor
{
  Options options;
  std::string path;
  Identity identity;
  std::vector<Column> columns;
  uint64_t rows = 0;
  std::string encode() const;
  int decode(const common::ObString &encoded);
};
struct Cell
{
  std::string text;
  bool is_null = false;
  bool quoted = false;
  Type type = Type::STRING;
};
int error(const char *message, uint64_t line = 0);
int check_enabled(ObSQLSessionInfo &session);
int secure_open(ObSQLSessionInfo &session, const std::string &path, bool directory, int &fd);
int parse_options(const ParseNode &node, Options &options, std::string &path);
int parse_copy(const ParseNode &node, Descriptor &desc);
int infer(ObSQLSessionInfo &session, Descriptor &desc, ObExecContext *ctx = nullptr);
common::ObObjType obj_type(Type type);
const char *type_name(Type type);
Type infer_text(const std::string &s, bool &wide);
Type merge_type(Type a, Type b);
const common::ObString *descriptor_expr(const ObRawExpr *expr);

// Holds one input record; consumers must copy strings before the next next().
// open/rescan validate the actual open descriptor and the original pathname.
class Reader
{
public:
  Reader() = default;
  ~Reader()
  {
    close();
  }
  int open(ObSQLSessionInfo &session, const Descriptor &desc, ObExecContext *ctx = nullptr);
  int next(std::vector<Cell> &row);
  int rescan();
  void set_projection(const std::vector<int64_t> &columns)
  {
    projection_ = columns;
    projected_ = true;
  }
  int verify();
  void close();
  const Descriptor &descriptor() const
  {
    return desc_;
  }
  uint64_t line() const
  {
    return record_line_;
  }

private:
  int get();
  int peek();
  int csv(std::vector<Cell> &row);
  int json(std::vector<Cell> &row, std::vector<std::string> *names = nullptr);
  int glob(std::vector<Cell> &row);
  int parquet(std::vector<Cell> &row);
  int next_record(std::vector<Cell> &row, std::vector<std::string> *names = nullptr);
  friend int infer(ObSQLSessionInfo &, Descriptor &, ObExecContext *);
  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;
  ObSQLSessionInfo *session_ = nullptr;
  ObExecContext *ctx_ = nullptr;
  Descriptor desc_;
  int fd_ = -1;
  void *parquet_ = nullptr;
  bool projected_ = false;
  std::vector<int64_t> projection_;
  void *dir_ = nullptr;
  std::string glob_dir_, glob_pattern_;
  char buffer_[65536];
  size_t pos_ = 0, end_ = 0;
  int io_error_ = 0;
  uint64_t physical_line_ = 1, record_line_ = 1, row_number_ = 0;
};
class CsvExport
{
public:
  ~CsvExport()
  {
    abort();
  }
  int open(ObSQLSessionInfo &session, const Descriptor &desc);
  int row(const std::vector<Cell> &cells);
  int finish(ObExecContext *ctx = nullptr);
  void abort();

private:
  int write(const std::string &s);
  ObSQLSessionInfo *session_ = nullptr;
  Descriptor desc_;
  int fd_ = -1, dir_fd_ = -1;
  std::string parent_, target_;
};
int cell_to_obj(const Cell &cell, Type type, common::ObObj &obj);
} // namespace file_sql
} // namespace sql
} // namespace oceanbase
