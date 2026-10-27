/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/file_scan_utils.h"
#include "sql/engine/basic/file_parquet_bridge.h"
#include "share/ob_server_struct.h"
#include "lib/charset/ob_charset.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/resolver/expr/ob_raw_expr.h"
#if defined(__linux__) && !defined(__ANDROID__)
#include <dirent.h>
#include <rapidjson/document.h>
#include <rapidjson/reader.h>
#include <rapidjson/memorystream.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <fast_float/fast_float.h>
#include <charconv>
#include <cmath>
#include <fcntl.h>
#include <fnmatch.h>
#include <unistd.h>
#include <limits.h>
#include <set>
#include <algorithm>

namespace oceanbase
{
namespace sql
{
namespace file_sql
{
using namespace common;
static bool utf8(const std::string &s);
// Match the resolver's identifier comparison, including its Unicode and accent
// equivalences. Bytewise ASCII folding can bind distinct input fields to one
// native column expression and silently return the wrong field.
struct NameLess
{
  bool operator()(const std::string &a, const std::string &b) const
  {
    return ObCharset::strcmp(CS_TYPE_UTF8MB4_GENERAL_CI, ObString(a.size(), a.data()),
                             ObString(b.size(), b.data())) < 0;
  }
};
static bool same_name(const std::string &a, const std::string &b)
{
  return ObCharset::case_insensitive_equal(ObString(a.size(), a.data()), ObString(b.size(), b.data()));
}
static std::string lower(std::string s)
{
  for (char &c : s)
  {
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  }
  return s;
}
int error(const char *message, uint64_t line)
{
  char diagnostic[512];
  snprintf(diagnostic, sizeof(diagnostic), "File SQL: %s (record starts at line %lu)", message, line);
  LOG_USER_ERROR(OB_INVALID_ARGUMENT, diagnostic);
  return OB_INVALID_ARGUMENT;
}
int check_enabled(ObSQLSessionInfo &session)
{
  ObObj value;
  int ret = session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, value);
  if (OB_SUCCESS == ret && value.get_int() == 0)
  {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "File SQL is disabled; set session enable_file_sql = ON");
  }
  return ret;
}
static bool contained(const std::string &root, const std::string &path)
{
  return root == "/" ||
         (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/') ||
         path == root;
}
int secure_open(ObSQLSessionInfo &session, const std::string &path, bool directory, int &fd)
{
  int ret = check_enabled(session);
  ObString priv;
  ObObj setting;
  ObArenaAllocator policy_allocator("FileSQLPolicy");
  char root[PATH_MAX], canonical[PATH_MAX], actual[PATH_MAX], fdpath[64];
  fd = -1;
  if (OB_SUCCESS != ret)
  {
  }
  else if (OB_SUCCESS != (ret = GCTX.schema_service_ != nullptr
                                    ? ObBasicSessionInfo::get_global_sys_variable(
                                          policy_allocator, session.get_dtc_params(),
                                          share::SYS_VAR_SECURE_FILE_PRIV, setting)
                                    : session.get_sys_variable(share::SYS_VAR_SECURE_FILE_PRIV, setting)))
  {
  }
  else if (!setting.is_null() && OB_SUCCESS != (ret = setting.get_string(priv)))
  {
  }
  else if (path.empty() || path.size() >= PATH_MAX || path.find('\0') != std::string::npos)
  {
    ret = error("invalid local path");
  }
  else if (priv.empty() || priv.case_compare("NULL") == 0)
  {
    ret = OB_ERR_NO_PRIVILEGE;
    LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "FILE (secure_file_priv is empty or NULL)");
  }
  else
  {
    std::string allowed(priv.ptr(), priv.length());
    struct stat allowed_stat;
    if (!realpath(allowed.c_str(), root) || !realpath(path.c_str(), canonical))
    {
      ret = error("cannot resolve file or secure_file_priv directory");
    }
    else if (stat(root, &allowed_stat) != 0 || !S_ISDIR(allowed_stat.st_mode))
    {
      ret = error("secure_file_priv must name an existing directory");
    }
    else if (!contained(root, canonical))
    {
      ret = error("path is outside secure_file_priv");
    }
    else
    {
      // O_NONBLOCK keeps FIFOs/devices from blocking before the regular-file check.
      fd = ::open(canonical, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW | (directory ? O_DIRECTORY : 0));
      struct stat st;
      if (fd < 0 || fstat(fd, &st) != 0 || (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)))
      {
        ret = error("only local regular files (or glob directories) are allowed");
      }
      else
      {
#ifdef __linux__
        snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(fdpath, actual, sizeof(actual) - 1);
        if (n < 0)
        {
          ret = error("cannot verify opened file identity");
        }
        else
        {
          actual[n] = 0;
          struct stat named;
          if (!contained(root, actual) || stat(path.c_str(), &named) != 0 || named.st_dev != st.st_dev ||
              named.st_ino != st.st_ino)
          {
            ret = error("file path changed during secure open");
          }
        }
#else
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "File SQL descriptor validation on this platform");
#endif
      }
    }
  }
  if (ret != OB_SUCCESS && fd >= 0)
  {
    ::close(fd);
    fd = -1;
  }
  return ret;
}
void Identity::assign(const struct stat &s)
{
  dev = s.st_dev;
  ino = s.st_ino;
  size = s.st_size;
  mtime = s.st_mtim.tv_sec;
  mtime_ns = s.st_mtim.tv_nsec;
  ctime = s.st_ctim.tv_sec;
  ctime_ns = s.st_ctim.tv_nsec;
}
bool Identity::matches(const struct stat &s) const
{
  return dev == s.st_dev && ino == s.st_ino && size == static_cast<uint64_t>(s.st_size) &&
         mtime == s.st_mtim.tv_sec && mtime_ns == s.st_mtim.tv_nsec && ctime == s.st_ctim.tv_sec &&
         ctime_ns == s.st_ctim.tv_nsec;
}
static bool string_node(const ParseNode *n)
{
  return n && (n->type_ == T_VARCHAR || n->type_ == T_CHAR);
}
int parse_options(const ParseNode &node, Options &o, std::string &path)
{
  o.format = node.value_;
  const ParseNode *args = node.children_[0];
  if (!args || args->num_child_ < 1 || !string_node(args->children_[0]))
    return error("path must be a literal string; prepared path parameters are unsupported");
  path.assign(args->children_[0]->str_value_, args->children_[0]->str_len_);
  std::set<std::string, NameLess> seen;
  for (int i = 1; i < args->num_child_; ++i)
  {
    const ParseNode *arg = args->children_[i];
    if (!arg || arg->type_ != T_OP_EQ || arg->num_child_ != 2)
      return OB_ERR_PARSE_SQL;
    const ParseNode *key = arg->children_[0], *value = arg->children_[1];
    if (key && key->type_ == T_COLUMN_REF)
    {
      if (key->num_child_ != 3 || key->children_[0] || key->children_[1])
        return error("named options require an unqualified identifier");
      key = key->children_[2];
    }
    if (!key || key->type_ != T_IDENT || !key->str_value_ || key->str_len_ <= 0 || !value)
      return error("expected named literal option");
    std::string name = lower(std::string(key->str_value_, key->str_len_));
    if (!seen.insert(name).second)
      return error("duplicate option");
    if (name == "sample_size" && o.format <= 2)
    {
      bool all = value->type_ == T_INT && value->value_ == -1;
      all |=
          value->type_ == T_OP_NEG && value->children_[0]->type_ == T_INT && value->children_[0]->value_ == 1;
      if (!all)
      {
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "File SQL sample_size other than -1");
        return OB_NOT_SUPPORTED;
      }
    }
    else if (name == "header" && o.format == 1)
    {
      if (value->type_ != T_BOOL)
        return error("header must be true or false");
      o.header = value->value_ != 0;
    }
    else if (name == "format" && o.format == 2)
    {
      if (!string_node(value) || std::string(value->str_value_, value->str_len_) != "newline_delimited")
        return error("only newline_delimited JSON is supported");
    }
    else if (o.format == 1 && (name == "delim" || name == "delimiter" || name == "quote" ||
                               name == "escape" || name == "nullstr"))
    {
      if (!string_node(value))
        return error("CSV option requires a literal string");
      std::string text(value->str_value_, value->str_len_);
      if (name == "nullstr")
      {
        if (text.size() > 1024)
          return error("nullstr too long");
        if (!utf8(text))
          return error("CSV NULL text must be UTF-8 without NUL bytes");
        o.nullstr = text;
      }
      else
      {
        if (text.size() != 1 || text[0] <= 0 || text[0] == '\r' || text[0] == '\n')
          return error("CSV control must be one ASCII character");
        if (name == "quote")
          o.quote = text[0];
        else if (name == "escape")
          o.escape = text[0];
        else
        {
          if (seen.count(name == "delim" ? "delimiter" : "delim"))
            return error("duplicate delimiter option");
          o.delimiter = text[0];
        }
      }
    }
    else
    {
      return error("unknown or unsupported option");
    }
  }
  if (o.delimiter == o.quote || o.delimiter == o.escape)
    return error("CSV delimiter must differ from quote and escape");
  return OB_SUCCESS;
}

int parse_copy(const ParseNode &node, Descriptor &desc)
{
  Options &o = desc.options;
  o.header = false;
  o.format = 5;
  const ParseNode *path = node.children_[0];
  if (!string_node(path))
    return error("COPY path must be a literal string");
  desc.path.assign(path->str_value_, path->str_len_);
  std::set<std::string, NameLess> seen;
  const ParseNode *args = node.children_[4];
  for (int i = 0; args && i < args->num_child_; ++i)
  {
    const auto *arg = args->children_[i];
    const auto *key = arg->children_[0];
    const auto *v = arg->children_[1];
    std::string name = lower(std::string(key->str_value_, key->str_len_));
    if (!seen.insert(name).second)
      return error("duplicate COPY option");
    if (name == "header")
    {
      if (v && v->type_ != T_BOOL)
        return error("COPY header requires a boolean");
      o.header = !v || v->value_ != 0;
    }
    else if (name == "format")
    {
      if (!v || lower(std::string(v->str_value_, v->str_len_)) != "csv")
        return error("COPY supports only FORMAT CSV");
    }
    else if (name == "delimiter" || name == "delim" || name == "quote" || name == "escape" ||
             name == "nullstr")
    {
      if (!string_node(v))
        return error("COPY CSV option must be a literal string");
      std::string text(v->str_value_, v->str_len_);
      if (name == "nullstr")
      {
        if (text.size() > 1024)
          return error("COPY nullstr too long");
        if (!utf8(text))
          return error("CSV NULL text must be UTF-8 without NUL bytes");
        o.nullstr = text;
      }
      else
      {
        if (text.size() != 1 || text[0] <= 0 || text[0] == '\r' || text[0] == '\n')
          return error("invalid COPY CSV control character");
        if (name == "quote")
          o.quote = text[0];
        else if (name == "escape")
          o.escape = text[0];
        else
        {
          if (seen.count(name == "delim" ? "delimiter" : "delim"))
            return error("duplicate delimiter");
          o.delimiter = text[0];
        }
      }
    }
    else
      return error("unknown COPY option");
  }
  if (o.delimiter == o.quote || o.delimiter == o.escape ||
      o.nullstr.find_first_of(std::string() + o.delimiter + o.quote + "\r\n") != std::string::npos)
    return error("ambiguous COPY CSV controls or NULL text");
  return OB_SUCCESS;
}
int CsvExport::open(ObSQLSessionInfo &session, const Descriptor &desc)
{
  abort();
  session_ = &session;
  desc_ = desc;
  if (!utf8(desc.options.nullstr))
    return error("CSV NULL text must be UTF-8 without NUL bytes");
  if (desc.path.empty() || desc.path.find('\0') != std::string::npos)
    return error("invalid COPY target");
  size_t slash = desc.path.find_last_of('/');
  parent_ = slash == std::string::npos ? "." : slash == 0 ? "/" : desc.path.substr(0, slash);
  target_ = desc.path.substr(slash == std::string::npos ? 0 : slash + 1);
  if (target_.empty() || target_ == "." || target_ == "..")
    return error("invalid COPY target basename");
  int ret = secure_open(session, parent_, true, dir_fd_);
  struct stat st;
  if (ret == OB_SUCCESS && fstatat(dir_fd_, target_.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0)
    ret = error("COPY target already exists");
  if (ret == OB_SUCCESS && errno != ENOENT)
    ret = error("cannot check COPY target");
  if (ret == OB_SUCCESS)
  {
#ifdef O_TMPFILE
    // No directory entry exists until publication. Neither a concurrent writer
    // nor cleanup can substitute or delete somebody else's temporary pathname.
    fd_ = openat(dir_fd_, ".", O_WRONLY | O_TMPFILE | O_CLOEXEC, 0600);
    if (fd_ < 0)
      ret = error("COPY requires a filesystem supporting safe O_TMPFILE creation");
#else
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY anonymous temporary files on this platform");
#endif
  }
  if (ret == OB_SUCCESS && desc.options.header)
  {
    std::vector<Cell> header;
    for (const auto &c : desc.columns)
    {
      Cell v;
      v.text = c.name;
      header.push_back(v);
    }
    ret = row(header);
  }
  if (ret != OB_SUCCESS)
    abort();
  return ret;
}
int CsvExport::write(const std::string &s)
{
  size_t pos = 0;
  while (pos < s.size())
  {
    ssize_t n = ::write(fd_, s.data() + pos, s.size() - pos);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return error("COPY write failed");
    pos += n;
  }
  return OB_SUCCESS;
}
int CsvExport::row(const std::vector<Cell> &cells)
{
  const auto &o = desc_.options;
  size_t bytes = 0;
  if (cells.size() > MAX_COLUMNS)
    return error("COPY column resource limit exceeded");
  for (const auto &cell : cells)
  {
    if (cell.text.size() > MAX_RECORD - bytes)
      return error("COPY record exceeds 8 MiB");
    bytes += cell.text.size();
  }
  std::string record;
  for (size_t i = 0; i < cells.size(); ++i)
  {
    if (i)
      record += o.delimiter;
    if (cells[i].is_null)
      record += o.nullstr;
    else
    {
      if (!utf8(cells[i].text))
        return error("COPY requires UTF-8 fields without NUL bytes");
      record += o.quote;
      for (char c : cells[i].text)
      {
        if (c == o.quote || (o.escape != o.quote && c == o.escape))
          record += o.escape;
        record += c;
        if (record.size() > 2 * MAX_RECORD)
          return error("COPY record resource limit exceeded");
      }
      record += o.quote;
    }
  }
  record += '\n';
  return write(record);
}
int CsvExport::finish(ObExecContext *ctx)
{
  int ret = ctx ? ctx->check_status() : OB_SUCCESS;
  if (ret == OB_SUCCESS)
    ret = check_enabled(*session_);
  int current = -1;
  if (ret == OB_SUCCESS)
    ret = secure_open(*session_, parent_, true, current);
  struct stat before, now;
  if (ret == OB_SUCCESS && (fstat(dir_fd_, &before) != 0 || fstat(current, &now) != 0 ||
                            before.st_dev != now.st_dev || before.st_ino != now.st_ino))
    ret = error("COPY directory changed");
  if (current >= 0)
    ::close(current);
  if (ret == OB_SUCCESS && fsync(fd_) != 0)
    ret = error("COPY flush failed");
  // Pin the original inode through a read-only descriptor. Publication must not
  // follow a temporary pathname that another directory writer can replace.
  int publish_fd = -1;
  char descriptor_path[64];
  if (ret == OB_SUCCESS)
  {
    snprintf(descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d", fd_);
    publish_fd = ::open(descriptor_path, O_RDONLY | O_CLOEXEC);
    if (publish_fd < 0)
      ret = error("cannot pin COPY temporary file");
  }
  if (fd_ >= 0)
  {
    if (::close(fd_) != 0 && ret == OB_SUCCESS)
      ret = error("COPY close failed");
    fd_ = -1;
  }
  if (ret == OB_SUCCESS && ctx)
    ret = ctx->check_status();
  if (ret == OB_SUCCESS)
  {
    snprintf(descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d", publish_fd);
    // linkat refuses an existing target atomically, including concurrent creates.
    if (linkat(AT_FDCWD, descriptor_path, dir_fd_, target_.c_str(), AT_SYMLINK_FOLLOW) != 0)
      ret = error("COPY publish failed (target may already exist)");
  }
  if (publish_fd >= 0)
    ::close(publish_fd);
  abort();
  return ret;
}
void CsvExport::abort()
{
  if (fd_ >= 0)
    ::close(fd_);
  if (dir_fd_ >= 0)
  {
    ::close(dir_fd_);
  }
  fd_ = dir_fd_ = -1;
}

std::string Descriptor::encode() const
{
  rapidjson::StringBuffer b;
  rapidjson::Writer<rapidjson::StringBuffer> w(b);
  w.StartArray();
  w.String("seekdb_file_sql_v1");
  w.Int(options.format);
  w.String(path.data(), path.size());
  w.Bool(options.header);
  w.Int(options.delimiter);
  w.Int(options.quote);
  w.Int(options.escape);
  w.String(options.nullstr.data(), options.nullstr.size());
  w.Uint64(identity.dev);
  w.Uint64(identity.ino);
  w.Uint64(identity.size);
  w.Int64(identity.mtime);
  w.Int64(identity.mtime_ns);
  w.Int64(identity.ctime);
  w.Int64(identity.ctime_ns);
  w.Uint64(rows);
  w.StartArray();
  for (const Column &c : columns)
  {
    w.StartArray();
    w.String(c.name.data(), c.name.size());
    w.Int(static_cast<int>(c.type));
    w.Bool(c.nullable);
    w.Int(c.scale);
    w.EndArray();
  }
  w.EndArray();
  w.EndArray();
  return std::string(b.GetString(), b.GetSize());
}
int Descriptor::decode(const ObString &s)
{
  if (s.length() > MAX_SCHEMA)
    return error("file metadata exceeds resource limit");
  rapidjson::Document d;
  d.Parse<rapidjson::kParseIterativeFlag>(s.ptr(), s.length());
  if (s.length() > MAX_SCHEMA || d.HasParseError() || !d.IsArray() || d.Size() != 17 || !d[0].IsString() ||
      std::string(d[0].GetString()) != "seekdb_file_sql_v1")
    return error("invalid file descriptor metadata");
  if (!d[1].IsInt() || d[1].GetInt() < 1 || d[1].GetInt() > 5 || !d[2].IsString() ||
      d[2].GetStringLength() >= PATH_MAX || !d[3].IsBool() || !d[7].IsString() ||
      d[7].GetStringLength() > 1024 || !d[16].IsArray() || d[16].Size() > MAX_COLUMNS)
    return error("invalid descriptor fields");
  for (int i = 4; i <= 6; ++i)
    if (!d[i].IsInt() || d[i].GetInt() <= 0 || d[i].GetInt() > 127)
      return error("invalid descriptor CSV control");
  for (int i = 8; i <= 10; ++i)
    if (!d[i].IsUint64())
      return error("invalid descriptor identity");
  for (int i = 11; i <= 14; ++i)
    if (!d[i].IsInt64())
      return error("invalid descriptor timestamps");
  if (!d[15].IsUint64())
    return error("invalid descriptor row count");
  options.format = d[1].GetInt();
  path.assign(d[2].GetString(), d[2].GetStringLength());
  options.header = d[3].GetBool();
  options.delimiter = d[4].GetInt();
  options.quote = d[5].GetInt();
  options.escape = d[6].GetInt();
  options.nullstr.assign(d[7].GetString(), d[7].GetStringLength());
  identity.dev = d[8].GetUint64();
  identity.ino = d[9].GetUint64();
  identity.size = d[10].GetUint64();
  identity.mtime = d[11].GetInt64();
  identity.mtime_ns = d[12].GetInt64();
  identity.ctime = d[13].GetInt64();
  identity.ctime_ns = d[14].GetInt64();
  rows = d[15].GetUint64();
  columns.clear();
  for (auto &v : d[16].GetArray())
  {
    if (!v.IsArray() || v.Size() != 4 || !v[0].IsString() || v[0].GetStringLength() > 256 || !v[1].IsInt() ||
        v[1].GetInt() < 0 || v[1].GetInt() > 6 || !v[2].IsBool() || !v[3].IsInt() || v[3].GetInt() < 0 ||
        v[3].GetInt() > 6)
      return error("invalid descriptor column");
    Column c;
    c.name.assign(v[0].GetString(), v[0].GetStringLength());
    c.type = static_cast<Type>(v[1].GetInt());
    c.nullable = v[2].GetBool();
    c.scale = v[3].GetInt();
    columns.push_back(c);
  }
  return OB_SUCCESS;
}
const ObString *descriptor_expr(const ObRawExpr *expr)
{
  if (!expr || !expr->is_const_raw_expr())
    return nullptr;
  const auto &v = static_cast<const ObConstRawExpr *>(expr)->get_value();
  // ObObj's string is returned by value, so use the const raw expression's literal storage.
  static thread_local ObString s;
  if (!v.is_varchar())
    return nullptr;
  s = v.get_string();
  static const char prefix[] = "[\"seekdb_file_sql_v1\",";
  return s.length() > sizeof(prefix) - 1 && memcmp(s.ptr(), prefix, sizeof(prefix) - 1) == 0 ? &s : nullptr;
}
ObObjType obj_type(Type t)
{
  switch (t)
  {
  case Type::INT:
    return ObIntType;
  case Type::DOUBLE:
    return ObDoubleType;
  case Type::BOOL:
    return ObTinyIntType;
  case Type::DATE:
    return ObDateType;
  case Type::DATETIME:
    return ObDateTimeType;
  default:
    return ObVarcharType;
  }
}
const char *type_name(Type t)
{
  switch (t)
  {
  case Type::INT:
    return "BIGINT";
  case Type::DOUBLE:
    return "DOUBLE";
  case Type::BOOL:
    return "BOOLEAN";
  case Type::DATE:
    return "DATE";
  case Type::DATETIME:
    return "DATETIME";
  default:
    return "VARCHAR";
  }
}
static bool integer(const std::string &s, int64_t &i)
{
  if (s.empty())
    return false;
  const char *start = s.data();
  if (*start == '+')
    ++start;
  auto result = std::from_chars(start, s.data() + s.size(), i);
  return result.ec == std::errc() && result.ptr == s.data() + s.size();
}
static bool floating(const std::string &s, double &v)
{
  if (s.empty())
    return false;
  const char *start = s.data();
  if (*start == '+')
    ++start;
  auto r = fast_float::from_chars(start, s.data() + s.size(), v);
  return r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(v);
}
// ISO civil values, independent of process locale and session time zone.
static bool temporal(const std::string &s, int64_t &micros, bool &datetime)
{
  if (s.size() != 10 && s.size() != 19 && !(s.size() >= 21 && s.size() <= 26))
    return false;
  auto number = [&](size_t p, size_t n) -> int
  {
    int v = 0;
    for (size_t j = p; j < p + n; ++j)
    {
      if (s[j] < '0' || s[j] > '9')
        return -1;
      v = v * 10 + s[j] - '0';
    }
    return v;
  };
  if (s[4] != '-' || s[7] != '-')
    return false;
  int y = number(0, 4), m = number(5, 2), d = number(8, 2);
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (y < 1 || m < 1 || m > 12 || d < 1 ||
      d > days[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)))
    return false;
  datetime = s.size() > 10;
  int h = 0, mi = 0, se = 0, us = 0;
  if (datetime)
  {
    if ((s[10] != 'T' && s[10] != ' ') || s[13] != ':' || s[16] != ':')
      return false;
    h = number(11, 2);
    mi = number(14, 2);
    se = number(17, 2);
    if (h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 59)
      return false;
    if (s.size() > 19)
    {
      if (s[19] != '.')
        return false;
      us = number(20, s.size() - 20);
      if (us < 0)
        return false;
      for (size_t j = s.size() - 20; j < 6; ++j)
        us *= 10;
    }
  }
  y -= m <= 2;
  const int era = y / 400;
  const unsigned yo = y - era * 400;
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t day = era * 146097 + yo * 365 + yo / 4 - yo / 100 + doy - 719468;
  micros = (day * 86400 + h * 3600 + mi * 60 + se) * 1000000 + us;
  return true;
}
Type infer_text(const std::string &s, bool &wide)
{
  if (s == "true" || s == "false" || s == "TRUE" || s == "FALSE")
    return Type::BOOL;
  int64_t i = 0;
  double v = 0;
  if (integer(s, i))
  {
    wide = i > 9007199254740992LL || i < -9007199254740992LL;
    return Type::INT;
  }
  // An integer outside BIGINT remains text; never round it through a double.
  bool digits = !s.empty();
  size_t p = (!s.empty() && (s[0] == '+' || s[0] == '-')) ? 1 : 0;
  for (size_t j = p; j < s.size(); ++j)
    digits &= s[j] >= '0' && s[j] <= '9';
  if (!digits && floating(s, v))
    return Type::DOUBLE;
  bool dt = false;
  if (temporal(s, i, dt))
    return dt ? Type::DATETIME : Type::DATE;
  return Type::STRING;
}
Type merge_type(Type a, Type b)
{
  if (a == Type::NIL)
    return b;
  if (b == Type::NIL || a == b)
    return a;
  if ((a == Type::DATE && b == Type::DATETIME) || (b == Type::DATE && a == Type::DATETIME))
    return Type::DATETIME;
  auto numeric = [](Type t) { return t == Type::INT || t == Type::BOOL || t == Type::DOUBLE; };
  if (numeric(a) && numeric(b))
    return a == Type::DOUBLE || b == Type::DOUBLE ? Type::DOUBLE : Type::INT;
  return Type::STRING;
}
static bool utf8(const std::string &s)
{
  const unsigned char *p = reinterpret_cast<const unsigned char *>(s.data());
  size_t i = 0;
  while (i < s.size())
  {
    unsigned c = p[i++];
    if (c == 0)
      return false;
    if (c < 128)
      continue;
    unsigned n = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : 99;
    if (n == 99 || i + n > s.size())
      return false;
    unsigned v = c & ((1 << (6 - n)) - 1);
    for (unsigned k = 0; k < n; ++k)
    {
      unsigned b = p[i++];
      if ((b & 0xC0) != 0x80)
        return false;
      v = (v << 6) | (b & 63);
    }
    if ((n == 2 && v < 0x800) || (n == 3 && v < 0x10000) || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
      return false;
  }
  return true;
}
int Reader::get()
{
  if (pos_ == end_)
  {
    ssize_t n;
    do
    {
      n = ::read(fd_, buffer_, sizeof(buffer_));
    } while (n < 0 && errno == EINTR);
    if (n < 0)
    {
      io_error_ = OB_IO_ERROR;
      return -1;
    }
    if (n == 0)
      return -1;
    pos_ = 0;
    end_ = n;
  }
  unsigned char c = buffer_[pos_++];
  if (c == '\n')
    ++physical_line_;
  return c;
}
int Reader::peek()
{
  int c = get();
  if (c >= 0)
  {
    --pos_;
    if (c == '\n')
      --physical_line_;
  }
  return c;
}
int Reader::csv(std::vector<Cell> &row)
{
  row.clear();
  record_line_ = physical_line_;
  if (peek() < 0)
    return io_error_ ? io_error_ : OB_ITER_END;
  size_t size = 0;
  bool done = false;
  while (!done)
  {
    Cell cell;
    cell.quoted = peek() == desc_.options.quote;
    if (cell.quoted)
    {
      get();
      bool closed = false;
      while (!closed)
      {
        int c = get();
        if (c < 0)
          return error("unterminated quoted CSV field", record_line_);
        if (c == desc_.options.escape && c != desc_.options.quote)
        {
          int next = get();
          if (next != desc_.options.quote && next != desc_.options.escape)
            return error("invalid CSV escape", physical_line_);
          cell.text += static_cast<char>(next);
        }
        else if (c == desc_.options.quote)
        {
          if (desc_.options.escape == desc_.options.quote && peek() == c)
          {
            get();
            cell.text += static_cast<char>(c);
          }
          else
            closed = true;
        }
        else
          cell.text += static_cast<char>(c);
        if (++size > MAX_RECORD)
          return error("CSV record exceeds 8 MiB", record_line_);
      }
      int c = peek();
      if (c >= 0 && c != desc_.options.delimiter && c != '\r' && c != '\n')
        return error("unexpected character after closing CSV quote", record_line_);
    }
    else
    {
      while (peek() >= 0 && peek() != desc_.options.delimiter && peek() != '\n' && peek() != '\r')
      {
        int c = get();
        if (c == desc_.options.quote)
          return error("quote in unquoted CSV field", record_line_);
        cell.text += static_cast<char>(c);
        if (++size > MAX_RECORD)
          return error("CSV record exceeds 8 MiB", record_line_);
      }
    }
    if (!utf8(cell.text))
      return error("invalid UTF-8 or NUL in CSV", record_line_);
    cell.is_null = !cell.quoted && cell.text == desc_.options.nullstr;
    row.push_back(std::move(cell));
    if (row.size() > MAX_COLUMNS)
      return error("too many CSV columns", record_line_);
    int c = get();
    if (c == desc_.options.delimiter)
      continue;
    if (c == '\r' && peek() == '\n')
      get();
    done = true;
  }
  return io_error_ ? io_error_ : OB_SUCCESS;
}
int Reader::json(std::vector<Cell> &row, std::vector<std::string> *names)
{
  std::string line;
  record_line_ = physical_line_;
  int ch;
  while ((ch = get()) >= 0 && ch != '\n')
  {
    line += static_cast<char>(ch);
    if (line.size() > MAX_RECORD)
      return error("JSON record exceeds 8 MiB", record_line_);
  }
  if (line.empty() && ch < 0)
    return io_error_ ? io_error_ : OB_ITER_END;
  if (!utf8(line))
    return error("invalid UTF-8 or NUL in JSON", record_line_);
  struct FlatObject : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, FlatObject>
  {
    std::vector<std::string> names;
    std::vector<Cell> cells;
    std::set<std::string, NameLess> seen;
    std::string key;
    int depth = 0;
    bool object = false;
    bool StartObject()
    {
      if (depth != 0 || object)
        return false;
      ++depth;
      object = true;
      return true;
    }
    bool EndObject(rapidjson::SizeType)
    {
      --depth;
      return depth == 0;
    }
    bool StartArray()
    {
      return false;
    }
    bool EndArray(rapidjson::SizeType)
    {
      return false;
    }
    bool Key(const char *s, rapidjson::SizeType n, bool)
    {
      key.assign(s, n);
      return depth == 1 && !key.empty() && key.size() <= 256 && utf8(key) && seen.insert(lower(key)).second &&
             names.size() < MAX_COLUMNS;
    }
    bool append(Cell c)
    {
      if (depth != 1 || key.empty())
        return false;
      names.push_back(key);
      cells.push_back(std::move(c));
      key.clear();
      return true;
    }
    bool Null()
    {
      Cell c;
      c.is_null = true;
      c.type = Type::NIL;
      return append(std::move(c));
    }
    bool Bool(bool v)
    {
      Cell c;
      c.text = v ? "true" : "false";
      c.type = Type::BOOL;
      return append(std::move(c));
    }
    bool String(const char *s, rapidjson::SizeType n, bool)
    {
      Cell c;
      c.text.assign(s, n);
      if (!utf8(c.text))
        return false;
      bool wide = false;
      c.type = infer_text(c.text, wide);
      if (c.type != Type::DATE && c.type != Type::DATETIME)
        c.type = Type::STRING;
      return append(std::move(c));
    }
    bool RawNumber(const char *s, rapidjson::SizeType n, bool)
    {
      Cell c;
      c.text.assign(s, n);
      bool wide = false;
      c.type = infer_text(c.text, wide);
      if (c.type == Type::STRING && c.text.find_first_of(".eE") != std::string::npos)
        return false;
      return append(std::move(c));
    }
  } handler;
  rapidjson::MemoryStream stream(line.data(), line.size());
  rapidjson::Reader parser;
  if (!parser.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseNumbersAsStringsFlag>(stream,
                                                                                                   handler) ||
      !handler.object || handler.depth != 0)
    return error("invalid JSONL: expected a flat object with unique keys and finite numbers", record_line_);
  row.clear();
  if (names)
  {
    *names = std::move(handler.names);
    row = std::move(handler.cells);
  }
  else
  {
    row.resize(desc_.columns.size());
    for (auto &c : row)
      c.is_null = true;
    for (size_t k = 0; k < handler.names.size(); ++k)
    {
      size_t i = 0;
      for (; i < desc_.columns.size(); ++i)
        if (same_name(desc_.columns[i].name, handler.names[k]))
          break;
      if (i == desc_.columns.size())
        return error("JSON schema changed after inference", record_line_);
      row[i] = std::move(handler.cells[k]);
    }
  }
  return OB_SUCCESS;
}
int Reader::glob(std::vector<Cell> &row)
{
  errno = 0;
  struct dirent *e;
  while ((e = readdir(static_cast<DIR *>(dir_))))
  {
    if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0)))
      continue;
    if (fnmatch(glob_pattern_.c_str(), e->d_name, FNM_PERIOD) != 0)
      continue;
    struct stat entry;
    if (fstatat(dirfd(static_cast<DIR *>(dir_)), e->d_name, &entry, AT_SYMLINK_NOFOLLOW) != 0)
      return error("glob entry changed during enumeration");
    if (S_ISDIR(entry.st_mode))
      continue;
    std::string path = glob_dir_ + "/" + e->d_name;
    int fd = -1;
    int ret = secure_open(*session_, path, false, fd);
    if (ret != OB_SUCCESS)
      return ret;
    ::close(fd);
    row.clear();
    Cell c;
    c.text = path;
    row.push_back(std::move(c));
    return OB_SUCCESS;
  }
  return errno ? OB_IO_ERROR : OB_ITER_END;
}
int Reader::parquet(std::vector<Cell> &row)
{
  if (!parquet_)
    return OB_NOT_INIT;
  char diagnostic[512] = {};
  int status = seekdb_parquet_next(parquet_, diagnostic, sizeof(diagnostic));
  if (status == 1)
    return OB_ITER_END;
  if (status != 0)
    return error(diagnostic);
  row.clear();
  row.resize(desc_.columns.size());
  size_t record_bytes = 0;
  for (size_t i = 0; i < row.size(); ++i)
  {
    bool is_null = false;
    const char *value = seekdb_parquet_cell(parquet_, i, &is_null, diagnostic, sizeof(diagnostic));
    if (!value && !is_null)
      return error(diagnostic);
    row[i].is_null = is_null;
    if (value)
    {
      record_bytes += strlen(value);
      if (record_bytes > MAX_RECORD)
        return error("Parquet record exceeds 8 MiB");
      row[i].text = value;
    }
  }
  return OB_SUCCESS;
}
int Reader::next_record(std::vector<Cell> &row, std::vector<std::string> *names)
{
  if (ctx_)
  {
    int ret = ctx_->check_status();
    if (ret != OB_SUCCESS)
      return ret;
  }
  if (desc_.options.format == 1)
    return csv(row);
  if (desc_.options.format == 2)
    return json(row, names);
  if (desc_.options.format == 4)
    return glob(row);
  return parquet(row);
}
static int parquet_check_status(void *context)
{
  return context ? static_cast<ObExecContext *>(context)->check_status() : OB_SUCCESS;
}
int Reader::open(ObSQLSessionInfo &session, const Descriptor &desc, ObExecContext *ctx)
{
  close();
  if (desc.options.format < 1 || desc.options.format > 4)
    return error("invalid input file format");
  session_ = &session;
  desc_ = desc;
  ctx_ = ctx;
  std::string path = desc.path;
  if (desc.options.format == 4)
  {
    size_t slash = path.find_last_of('/');
    glob_dir_ = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
    glob_pattern_ = path.substr(slash == std::string::npos ? 0 : slash + 1);
    if (glob_dir_.find_first_of("*?[") != std::string::npos)
      return error("glob supports patterns only in the final path component");
    path = glob_dir_;
  }
  int ret = secure_open(session, path, desc.options.format == 4, fd_);
  if (ret == OB_SUCCESS && desc_.identity.ino == 0)
  {
    struct stat initial;
    if (fstat(fd_, &initial) != 0)
      ret = OB_IO_ERROR;
    else
      desc_.identity.assign(initial);
  }
  if (ret == OB_SUCCESS && desc.options.format == 4)
  {
    dir_ = fdopendir(fd_);
    if (!dir_)
    {
      ret = OB_IO_ERROR;
    }
    else
      fd_ = -1;
  }
  if (ret == OB_SUCCESS && desc.options.format != 4 && desc.identity.ino != 0)
    ret = verify();
  if (ret == OB_SUCCESS && desc.options.format == 3)
  {
    char diagnostic[512] = {};
    int duplicate = dup(fd_);
    if (duplicate < 0)
      ret = OB_IO_ERROR;
    else
    {
      parquet_ = seekdb_parquet_open(duplicate, parquet_check_status, ctx_, diagnostic, sizeof(diagnostic));
      if (!parquet_)
      {
        ret = ctx_ ? ctx_->check_status() : OB_SUCCESS;
        if (ret == OB_SUCCESS)
          ret = error(diagnostic);
      }
      else if (!desc.columns.empty())
      {
        std::vector<int64_t> columns = projection_;
        if (!projected_)
          for (size_t i = 0; i < desc.columns.size(); ++i)
            columns.push_back(i);
        if (seekdb_parquet_start(parquet_, columns.data(), columns.size(), diagnostic, sizeof(diagnostic)) !=
            0)
          ret = error(diagnostic);
      }
    }
  }
  if (ret == OB_SUCCESS && desc.options.format == 1 && desc.options.header)
  {
    std::vector<Cell> header;
    ret = csv(header);
    if (ret == OB_ITER_END)
      ret = OB_SUCCESS;
  }
  if (ret != OB_SUCCESS)
    close();
  return ret;
}
int Reader::verify()
{
  int ret = check_enabled(*session_);
  if (ret != OB_SUCCESS)
    return ret;
  if (desc_.options.format == 4)
  {
    int current = -1;
    ret = secure_open(*session_, glob_dir_, true, current);
    struct stat opened, named;
    if (ret == OB_SUCCESS &&
        (!dir_ || fstat(dirfd(static_cast<DIR *>(dir_)), &opened) != 0 || fstat(current, &named) != 0 ||
         opened.st_dev != named.st_dev || opened.st_ino != named.st_ino ||
         desc_.identity.dev != opened.st_dev || desc_.identity.ino != opened.st_ino))
      ret = error("glob directory changed");
    if (current >= 0)
      ::close(current);
    return ret;
  }
  struct stat st, named;
  if (fd_ < 0 || fstat(fd_, &st) != 0 || stat(desc_.path.c_str(), &named) != 0 ||
      !desc_.identity.matches(st) || !desc_.identity.matches(named))
    return error("input file changed since schema inference", record_line_);
  // Recheck current permission and path resolution as well as inode metadata.
  int fd = -1;
  ret = secure_open(*session_, desc_.path, false, fd);
  if (fd >= 0)
    ::close(fd);
  return ret;
}
int Reader::next(std::vector<Cell> &row)
{
  int ret = OB_SUCCESS;
  if (row_number_ % 128 == 0 && OB_SUCCESS != (ret = verify()))
    return ret;
  ret = next_record(row);
  if (ret == OB_SUCCESS)
  {
    ++row_number_;
    if (row.size() != desc_.columns.size())
      return error("CSV field count differs from schema", record_line_);
  }
  else if (ret == OB_ITER_END)
  {
    int check = verify();
    if (check != OB_SUCCESS)
      ret = check;
  }
  return ret;
}
int Reader::rescan()
{
  if (!session_)
    return OB_NOT_INIT;
  auto *s = session_;
  auto d = desc_;
  auto *c = ctx_;
  return open(*s, d, c);
}
void Reader::close()
{
  if (parquet_)
  {
    seekdb_parquet_close(parquet_);
    parquet_ = nullptr;
  }
  if (fd_ >= 0)
    ::close(fd_);
  if (dir_)
    closedir(static_cast<DIR *>(dir_));
  fd_ = -1;
  dir_ = nullptr;
  pos_ = end_ = 0;
  io_error_ = 0;
  physical_line_ = record_line_ = 1;
  row_number_ = 0;
}
int infer(ObSQLSessionInfo &session, Descriptor &desc, ObExecContext *ctx)
{
  Reader r;
  Descriptor input = desc;
  input.options.header = false;
  int ret = r.open(session, input, ctx);
  if (ret != OB_SUCCESS)
    return ret;
  desc.identity = r.desc_.identity;
  if (desc.options.format == 4)
  {
    Column c;
    c.name = "file";
    c.type = Type::STRING;
    desc.columns.push_back(c);
    return OB_SUCCESS;
  }
  if (desc.options.format == 3)
  {
    desc.rows = seekdb_parquet_rows(r.parquet_);
    std::set<std::string, NameLess> used_names;
    for (size_t i = 0; i < seekdb_parquet_columns(r.parquet_); ++i)
    {
      int32_t type = 0;
      bool nullable = false;
      const char *name = seekdb_parquet_column(r.parquet_, i, &type, &nullable);
      Column c;
      c.name = name;
      if (!used_names.insert(c.name).second)
        return error("Parquet column names conflict under SQL identifier comparison");
      c.type = static_cast<Type>(type);
      c.nullable = nullable || c.type == Type::NIL;
      if (c.type == Type::NIL)
        c.type = Type::STRING;
      c.scale = c.type == Type::DATETIME ? 6 : 0;
      desc.columns.push_back(c);
    }
    return r.verify();
  }
  std::vector<Cell> row;
  std::vector<std::string> names;
  std::set<std::string, NameLess> used;
  size_t schema_bytes = 0;
  if (desc.options.format == 1)
  {
    ret = r.csv(row);
    if (ret == OB_ITER_END)
      return error("cannot infer columns from an empty CSV file");
    if (ret != OB_SUCCESS)
      return ret;
    for (size_t i = 0; i < row.size(); ++i)
    {
      Column c;
      std::string base = desc.options.header ? row[i].text : "column" + std::to_string(i);
      if (i == 0 && base.compare(0, 3, "\xEF\xBB\xBF") == 0)
        base.erase(0, 3);
      if (base.empty())
        base = "column" + std::to_string(i);
      if (base.size() > 256)
        return error("column name exceeds 256 bytes");
      c.name = base;
      int suffix = 2;
      while (!used.insert(lower(c.name)).second)
        c.name = base + "__" + std::to_string(suffix++);
      if (c.name.size() > 256)
        return error("deduplicated column name exceeds 256 bytes");
      schema_bytes += c.name.size();
      desc.columns.push_back(c);
    }
    if (!desc.options.header)
    {
      r.pos_ = r.end_ = 0;
      r.physical_line_ = 1;
      if (lseek(r.fd_, 0, SEEK_SET) < 0)
        return OB_IO_ERROR;
    }
  }
  while ((ret = r.next_record(row, desc.options.format == 2 ? &names : nullptr)) == OB_SUCCESS)
  {
    if (desc.options.format == 2)
    {
      std::vector<Cell> ordered(desc.columns.size());
      for (auto &c : ordered)
        c.is_null = true;
      for (size_t k = 0; k < names.size(); ++k)
      {
        size_t i = 0;
        for (; i < desc.columns.size(); ++i)
          if (same_name(desc.columns[i].name, names[k]))
            break;
        if (i == desc.columns.size())
        {
          Column c;
          c.name = names[k];
          c.nullable = desc.rows > 0;
          schema_bytes += c.name.size();
          desc.columns.push_back(c);
          Cell missing;
          missing.is_null = true;
          ordered.push_back(missing);
        }
        ordered[i] = std::move(row[k]);
      }
      row = std::move(ordered);
    }
    if (row.size() != desc.columns.size())
      return error("CSV record has inconsistent field count", r.line());
    if (desc.columns.size() > MAX_COLUMNS || schema_bytes > MAX_SCHEMA / 2)
      return error("schema resource limit exceeded", r.line());
    for (size_t i = 0; i < row.size(); ++i)
    {
      Column &c = desc.columns[i];
      const Cell &v = row[i];
      ++c.seen;
      if (v.is_null)
        c.nullable = true;
      else
      {
        bool wide = false;
        Type t = desc.options.format == 1 ? infer_text(v.text, wide) : v.type;
        if (desc.options.format == 2 && t == Type::INT)
        {
          int64_t val;
          integer(v.text, val);
          wide = val > 9007199254740992LL || val < -9007199254740992LL;
        }
        if (t == Type::DATETIME && v.text.size() > 19)
          c.scale = std::max(c.scale, static_cast<int>(v.text.size() - 20));
        c.wide_int |= wide;
        c.type = merge_type(c.type, t);
        if (c.wide_int && c.type == Type::DOUBLE)
          c.type = Type::STRING;
      }
    }
    ++desc.rows;
    if (desc.rows % 128 == 0 && (ret = r.verify()) != OB_SUCCESS)
      return ret;
  }
  if (ret != OB_ITER_END)
    return ret;
  if (desc.columns.empty())
    return error("cannot infer columns from empty JSONL");
  for (auto &c : desc.columns)
  {
    if (c.type == Type::NIL)
      c.type = Type::STRING;
    if (desc.rows == 0)
      c.nullable = true;
  }
  return r.verify();
}
int cell_to_obj(const Cell &c, Type type, ObObj &obj)
{
  if (c.is_null)
  {
    obj.set_null();
    return OB_SUCCESS;
  }
  int64_t i = 0;
  double v = 0;
  bool dt = false;
  switch (type)
  {
  case Type::INT:
    if (c.text == "true" || c.text == "TRUE")
      i = 1;
    else if (c.text == "false" || c.text == "FALSE")
      i = 0;
    else if (!integer(c.text, i))
      return error("invalid BIGINT during scan");
    obj.set_int(i);
    break;
  case Type::BOOL:
    obj.set_tinyint(c.text == "true" || c.text == "TRUE");
    break;
  case Type::DOUBLE:
    if (c.text == "true" || c.text == "TRUE")
      v = 1;
    else if (c.text == "false" || c.text == "FALSE")
      v = 0;
    else if (!floating(c.text, v))
      return error("invalid DOUBLE during scan");
    obj.set_double(v);
    break;
  case Type::DATE:
  case Type::DATETIME:
    if (!temporal(c.text, i, dt))
      return error("invalid ISO date or datetime during scan");
    if (type == Type::DATE)
      obj.set_date(static_cast<int32_t>(i / 86400000000LL));
    else
      obj.set_datetime(i);
    break;
  default:
    obj.set_varchar(ObString(c.text.size(), c.text.data()));
    obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    break;
  }
  return OB_SUCCESS;
}
} // namespace file_sql
} // namespace sql
} // namespace oceanbase

#else
// Keep existing SQL builds usable on other platforms. File SQL is Linux-only
// until equivalent descriptor validation and atomic publication are provided.
namespace oceanbase
{
namespace sql
{
namespace file_sql
{
using namespace common;
static int unavailable()
{
  LOG_USER_ERROR(OB_NOT_SUPPORTED, "File SQL on this platform (Linux is required)");
  return OB_NOT_SUPPORTED;
}
int error(const char *, uint64_t)
{
  return unavailable();
}
int check_enabled(ObSQLSessionInfo &)
{
  return unavailable();
}
int secure_open(ObSQLSessionInfo &, const std::string &, bool, int &fd)
{
  fd = -1;
  return unavailable();
}
void Identity::assign(const struct stat &) {}
bool Identity::matches(const struct stat &) const
{
  return false;
}
std::string Descriptor::encode() const
{
  return {};
}
int Descriptor::decode(const ObString &)
{
  return unavailable();
}
int parse_options(const ParseNode &, Options &, std::string &)
{
  return unavailable();
}
int parse_copy(const ParseNode &, Descriptor &)
{
  return unavailable();
}
int infer(ObSQLSessionInfo &, Descriptor &, ObExecContext *)
{
  return unavailable();
}
ObObjType obj_type(Type)
{
  return ObVarcharType;
}
const char *type_name(Type)
{
  return "VARCHAR";
}
Type infer_text(const std::string &, bool &)
{
  return Type::STRING;
}
Type merge_type(Type, Type)
{
  return Type::STRING;
}
const ObString *descriptor_expr(const ObRawExpr *)
{
  return nullptr;
}
int Reader::open(ObSQLSessionInfo &, const Descriptor &, ObExecContext *)
{
  return unavailable();
}
int Reader::next(std::vector<Cell> &)
{
  return unavailable();
}
int Reader::rescan()
{
  return unavailable();
}
int Reader::verify()
{
  return unavailable();
}
void Reader::close() {}
int CsvExport::open(ObSQLSessionInfo &, const Descriptor &)
{
  return unavailable();
}
int CsvExport::row(const std::vector<Cell> &)
{
  return unavailable();
}
int CsvExport::finish(ObExecContext *)
{
  return unavailable();
}
void CsvExport::abort() {}
int cell_to_obj(const Cell &, Type, ObObj &)
{
  return unavailable();
}
} // namespace file_sql
} // namespace sql
} // namespace oceanbase
#endif
