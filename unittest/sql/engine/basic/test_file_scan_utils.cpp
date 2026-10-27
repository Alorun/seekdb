/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#if defined(__linux__) && !defined(__ANDROID__)
#include <gtest/gtest.h>
#include <dirent.h>
#include <fstream>
#include <unistd.h>
#include "sql/engine/basic/file_scan_utils.h"
#include "sql/session/ob_sql_session_info.h"
using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::sql;
using namespace oceanbase::sql::file_sql;

class FileScanUtils : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;
  ObSQLSessionInfo session_;
  std::string dir_;
  void SetUp() override
  {
    char name[] = "/tmp/seekdb-file-test-XXXXXX";
    ASSERT_NE(nullptr, mkdtemp(name));
    dir_ = name;
    ASSERT_EQ(OB_SUCCESS, session_.test_init(1, 100, &allocator_));
    ASSERT_EQ(OB_SUCCESS,
              session_.load_sys_variable(
                  allocator_, ObString("enable_file_sql"), ObIntType, ObString("0"), ObString(), ObString(),
                  share::ObSysVarFlag::SESSION_SCOPE | share::ObSysVarFlag::GLOBAL_SCOPE, false));
    ASSERT_EQ(OB_SUCCESS,
              session_.load_sys_variable(
                  allocator_, ObString("secure_file_priv"), ObVarcharType, ObString(dir_.c_str()), ObString(),
                  ObString(), share::ObSysVarFlag::GLOBAL_SCOPE | share::ObSysVarFlag::NULLABLE, false));
    ASSERT_EQ(OB_SUCCESS, session_.update_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, int64_t(1)));
    ASSERT_EQ(OB_SUCCESS,
              session_.update_sys_variable(share::SYS_VAR_SECURE_FILE_PRIV, ObString(dir_.c_str())));
  }
  void TearDown() override
  {
    DIR *d = opendir(dir_.c_str());
    if (d)
    {
      while (auto *e = readdir(d))
      {
        std::string n = e->d_name;
        if (n != "." && n != "..")
          unlink((dir_ + "/" + n).c_str());
      }
      closedir(d);
    }
    rmdir(dir_.c_str());
  }
  std::string put(const char *name, const std::string &data)
  {
    std::string path = dir_ + "/" + name;
    std::ofstream f(path, std::ios::binary);
    f << data;
    return path;
  }
};
TEST(FileSqlTypes, numeric_date_boundaries)
{
  struct Case
  {
    const char *text;
    Type type;
  } cases[] = {{"9223372036854775807", Type::INT},
               {"-9223372036854775808", Type::INT},
               {"9223372036854775808", Type::STRING},
               {"-9223372036854775809", Type::STRING},
               {"1e30", Type::DOUBLE},
               {"1e309", Type::STRING},
               {"1e-999", Type::STRING},
               {"2024-02-29", Type::DATE},
               {"1900-02-29", Type::STRING},
               {"2000-02-29", Type::DATE},
               {"2026-13-01", Type::STRING},
               {"2026-01-01T23:59:59.123456", Type::DATETIME},
               {"2026-01-01 24:00:00", Type::STRING},
               {"true", Type::BOOL},
               {"", Type::STRING}};
  for (const auto &c : cases)
  {
    bool wide = false;
    EXPECT_EQ(c.type, infer_text(c.text, wide)) << c.text;
  }
  EXPECT_EQ(Type::DATETIME, merge_type(Type::DATE, Type::DATETIME));
  EXPECT_EQ(Type::STRING, merge_type(Type::BOOL, Type::DATE));
}
TEST_F(FileScanUtils, csv_multiline_boundary_rescan_and_null)
{
  Descriptor d;
  std::string long_text(65530, 'x');
  d.path = put("input.csv", "id,id,note,empty,nullcol\r\n1,2,\"" + long_text +
                                "\nhello,\"\"world\"\"\",\"\",\r\n3,4,plain,\"NULL\",\r\n");
  ASSERT_EQ(OB_SUCCESS, infer(session_, d));
  ASSERT_EQ(5, d.columns.size());
  EXPECT_EQ("id__2", d.columns[1].name);
  EXPECT_EQ(Type::STRING, d.columns[4].type);
  EXPECT_TRUE(d.columns[4].nullable);
  Reader r;
  ASSERT_EQ(OB_SUCCESS, r.open(session_, d));
  std::vector<Cell> row;
  ASSERT_EQ(OB_SUCCESS, r.next(row));
  EXPECT_EQ(long_text + "\nhello,\"world\"", row[2].text);
  EXPECT_FALSE(row[3].is_null);
  EXPECT_TRUE(row[4].is_null);
  ASSERT_EQ(OB_SUCCESS, r.next(row));
  EXPECT_EQ("3", row[0].text);
  ASSERT_EQ(OB_ITER_END, r.next(row));
  ASSERT_EQ(OB_SUCCESS, r.rescan());
  ASSERT_EQ(OB_SUCCESS, r.next(row));
  EXPECT_EQ("1", row[0].text);
}
TEST_F(FileScanUtils, inference_reaches_last_record_and_is_order_independent)
{
  Descriptor d;
  d.path = put("late.csv", "v\n" + std::string("1\n") + "9007199254740993\n1.5\n");
  ASSERT_EQ(OB_SUCCESS, infer(session_, d));
  EXPECT_EQ(Type::STRING, d.columns[0].type);
  Descriptor j;
  j.options.format = 2;
  j.path = put("late.jsonl",
               "{\"id\":1}\n{\"late\":true,\"id\":null}\n{\"id\":\"text\",\"last\":\"2024-02-29\"}\n");
  ASSERT_EQ(OB_SUCCESS, infer(session_, j));
  ASSERT_EQ(3, j.columns.size());
  EXPECT_EQ("id", j.columns[0].name);
  EXPECT_EQ("late", j.columns[1].name);
  EXPECT_EQ("last", j.columns[2].name);
  EXPECT_EQ(Type::STRING, j.columns[0].type);
  EXPECT_TRUE(j.columns[1].nullable);
  Reader r;
  ASSERT_EQ(OB_SUCCESS, r.open(session_, j));
  std::vector<Cell> row;
  ASSERT_EQ(OB_SUCCESS, r.next(row));
  EXPECT_TRUE(row[1].is_null);
  ASSERT_EQ(OB_SUCCESS, r.next(row));
  EXPECT_TRUE(row[0].is_null);
}
TEST_F(FileScanUtils, malformed_records_and_resource_limits)
{
  for (const auto &text : {"a,b\n1\n", "a\n\"unterminated\n", "a\n\"ok\"bad\n", "a\n\xC0\x80\n"})
  {
    Descriptor d;
    d.path = put("bad.csv", text);
    EXPECT_NE(OB_SUCCESS, infer(session_, d));
  }
  for (const auto &text : {"[]\n", "{\"a\":{}}\n", "{\"a\":1,\"a\":2}\n", "\n", "{bad}\n"})
  {
    Descriptor d;
    d.options.format = 2;
    d.path = put("bad.jsonl", text);
    EXPECT_NE(OB_SUCCESS, infer(session_, d));
  }
  Descriptor d;
  d.path = put("large.csv", "a\n\"" + std::string(MAX_RECORD + 1, 'x') + "\"\n");
  EXPECT_NE(OB_SUCCESS, infer(session_, d));
}
TEST_F(FileScanUtils, disabled_escape_symlink_and_file_changes)
{
  Descriptor d;
  d.path = put("safe.csv", "a\n1\n");
  ASSERT_EQ(OB_SUCCESS, infer(session_, d));
  ASSERT_EQ(0, symlink("/etc/passwd", (dir_ + "/outside").c_str()));
  int fd = -1;
  EXPECT_NE(OB_SUCCESS, secure_open(session_, dir_ + "/outside", false, fd));
  EXPECT_EQ(-1, fd);
  Reader r;
  ASSERT_EQ(OB_SUCCESS, r.open(session_, d));
  put("safe.csv", "a\n2\n");
  std::vector<Cell> row;
  EXPECT_NE(OB_SUCCESS, r.next(row));
  EXPECT_NE(OB_SUCCESS, r.rescan());
  ASSERT_EQ(OB_SUCCESS, session_.update_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, int64_t(0)));
  Descriptor disabled;
  disabled.path = d.path;
  EXPECT_EQ(OB_NOT_SUPPORTED, infer(session_, disabled));
}
TEST_F(FileScanUtils, copy_roundtrip_no_replace_and_cleanup)
{
  Descriptor d;
  d.path = dir_ + "/export.csv";
  d.options.header = true;
  d.options.nullstr = "NULL";
  Column col;
  col.name = "v";
  d.columns.push_back(col);
  CsvExport writer;
  ASSERT_EQ(OB_SUCCESS, writer.open(session_, d));
  std::vector<Cell> row(1);
  row[0].text = "a,\"b\"\r\nc";
  ASSERT_EQ(OB_SUCCESS, writer.row(row));
  row[0].text = "";
  ASSERT_EQ(OB_SUCCESS, writer.row(row));
  row[0].is_null = true;
  ASSERT_EQ(OB_SUCCESS, writer.row(row));
  row[0].is_null = false;
  row[0].text = "NULL";
  ASSERT_EQ(OB_SUCCESS, writer.row(row));
  ASSERT_EQ(OB_SUCCESS, writer.finish());
  Descriptor input;
  input.path = d.path;
  input.options.nullstr = "NULL";
  ASSERT_EQ(OB_SUCCESS, infer(session_, input));
  Reader r;
  ASSERT_EQ(OB_SUCCESS, r.open(session_, input));
  ASSERT_EQ(OB_SUCCESS, r.next(row));
  EXPECT_EQ("a,\"b\"\r\nc", row[0].text);
  ASSERT_EQ(OB_SUCCESS, r.next(row));
  EXPECT_FALSE(row[0].is_null);
  EXPECT_EQ("", row[0].text);
  ASSERT_EQ(OB_SUCCESS, r.next(row));
  EXPECT_TRUE(row[0].is_null);
  ASSERT_EQ(OB_SUCCESS, r.next(row));
  EXPECT_EQ("NULL", row[0].text);
  EXPECT_FALSE(row[0].is_null);
  CsvExport again;
  EXPECT_NE(OB_SUCCESS, again.open(session_, d));
  Descriptor race = d;
  race.path = dir_ + "/race.csv";
  CsvExport competing;
  ASSERT_EQ(OB_SUCCESS, competing.open(session_, race));
  put("race.csv", "keep");
  EXPECT_NE(OB_SUCCESS, competing.finish());
  std::ifstream f(race.path);
  std::string keep;
  f >> keep;
  EXPECT_EQ("keep", keep);
  Descriptor limit = d;
  limit.path = dir_ + "/limit.csv";
  CsvExport limited;
  ASSERT_EQ(OB_SUCCESS, limited.open(session_, limit));
  row.resize(2);
  for (auto &cell : row)
  {
    cell.is_null = false;
    cell.text.assign(MAX_RECORD / 2 + 1, 'x');
  }
  EXPECT_NE(OB_SUCCESS, limited.row(row));
  limited.abort();
  EXPECT_NE(0, access(limit.path.c_str(), F_OK));
}

TEST_F(FileScanUtils, denies_null_privilege_special_files_and_prefix_siblings)
{
  Descriptor d;
  d.path = put("allowed.csv", "a\n1\n");
  ASSERT_EQ(0, mkfifo((dir_ + "/fifo").c_str(), 0600));
  int fd = -1;
  EXPECT_NE(OB_SUCCESS, secure_open(session_, dir_ + "/fifo", false, fd));
  EXPECT_EQ(-1, fd);
  ASSERT_EQ(0, symlink(d.path.c_str(), (dir_ + "/inside").c_str()));
  ASSERT_EQ(OB_SUCCESS, secure_open(session_, dir_ + "/inside", false, fd));
  close(fd);
  std::string sibling = dir_ + "-sibling";
  ASSERT_EQ(0, mkdir(sibling.c_str(), 0700));
  std::string outside = sibling + "/data.csv";
  {
    std::ofstream f(outside);
    f << "a\n1\n";
  }
  EXPECT_NE(OB_SUCCESS, secure_open(session_, outside, false, fd));
  unlink(outside.c_str());
  rmdir(sibling.c_str());
  ObObj denied;
  denied.set_null();
  ASSERT_EQ(OB_SUCCESS, session_.update_sys_variable(share::SYS_VAR_SECURE_FILE_PRIV, denied));
  EXPECT_NE(OB_SUCCESS, infer(session_, d));
}

TEST_F(FileScanUtils, detects_inode_replacement_and_releases_rescan_handles)
{
  Descriptor d;
  d.path = put("replace.csv", "a\n1\n");
  ASSERT_EQ(OB_SUCCESS, infer(session_, d));
  auto descriptors = []()
  {
    size_t n = 0;
    DIR *directory = opendir("/proc/self/fd");
    while (readdir(directory))
      ++n;
    closedir(directory);
    return n;
  };
  const size_t before = descriptors();
  CsvExport existing;
  for (int i = 0; i < 100; ++i)
    EXPECT_NE(OB_SUCCESS, existing.open(session_, d));
  EXPECT_EQ(before, descriptors());
  {
    Reader reader;
    ASSERT_EQ(OB_SUCCESS, reader.open(session_, d));
    std::vector<Cell> row;
    for (int i = 0; i < 100; ++i)
    {
      ASSERT_EQ(OB_SUCCESS, reader.next(row));
      ASSERT_EQ(OB_SUCCESS, reader.rescan());
    }
    std::string replacement = put("new.csv", "a\n1\n");
    ASSERT_EQ(0, rename(replacement.c_str(), d.path.c_str()));
    EXPECT_NE(OB_SUCCESS, reader.next(row));
    EXPECT_NE(OB_SUCCESS, reader.rescan());
  }
  EXPECT_EQ(before, descriptors());
}

TEST_F(FileScanUtils, identifiers_follow_native_unicode_comparison)
{
  Descriptor csv;
  csv.path = put("unicode.csv", "É,e\n1,2\n");
  ASSERT_EQ(OB_SUCCESS, infer(session_, csv));
  ASSERT_EQ(2, csv.columns.size());
  EXPECT_EQ("É", csv.columns[0].name);
  EXPECT_EQ("e__2", csv.columns[1].name);
  Descriptor json;
  json.options.format = 2;
  json.path = put("unicode.jsonl", "{\"É\":1}\n{\"e\":2}\n");
  ASSERT_EQ(OB_SUCCESS, infer(session_, json));
  ASSERT_EQ(1, json.columns.size());
  Reader reader;
  ASSERT_EQ(OB_SUCCESS, reader.open(session_, json));
  std::vector<Cell> row;
  ASSERT_EQ(OB_SUCCESS, reader.next(row));
  ASSERT_EQ(OB_SUCCESS, reader.next(row));
  EXPECT_EQ("2", row[0].text);
  Descriptor conflicting;
  conflicting.options.format = 2;
  conflicting.path = put("conflict.jsonl", "{\"É\":1,\"e\":2}\n");
  EXPECT_NE(OB_SUCCESS, infer(session_, conflicting));
}

#endif
