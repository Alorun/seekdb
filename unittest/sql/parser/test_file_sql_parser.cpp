/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "sql/parser/ob_parser.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;
TEST(FileSqlParser, accepts_native_file_sources_and_copy)
{
  const char *queries[] = {
      "select * from read_csv('/tmp/x.csv')",
      "select f.id from read_csv('/tmp/x', header = true, sample_size = -1) as f",
      "select * from read_json('/tmp/x', format = 'newline_delimited') j",
      "select * from read_ndjson('/tmp/x')",
      "select * from read_parquet('/tmp/x') p left join read_csv('/tmp/y') c on p.id=c.id",
      "select file from glob('/tmp/*')",
      "describe select * from read_csv('/tmp/x')",
      "copy (select id from read_csv('/tmp/x') order by id) to '/tmp/y' (format csv, header, nullstr 'NULL')",
      "copy test.t to '/tmp/y' (delimiter '|', quote '\"', escape '\"', header false)",
      "select 1+2 as normal_sql"};
  for (const char *sql : queries)
  {
    ObArenaAllocator allocator;
    ObParser parser(allocator, SMO_DEFAULT);
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString(sql), result)) << sql;
    ASSERT_NE(nullptr, result.result_tree_);
  }
}
TEST(FileSqlParser, rejects_malformed_file_syntax)
{
  const char *queries[] = {"select * from read_csv()", "select * from read_csv('/tmp/x',)",
                           "copy (select 1) to 123", "copy (select 1) to '/tmp/x' (header,)"};
  for (const char *sql : queries)
  {
    ObArenaAllocator allocator;
    ObParser parser(allocator, SMO_DEFAULT);
    ParseResult result;
    EXPECT_NE(OB_SUCCESS, parser.parse(ObString(sql), result)) << sql;
  }
}

#include "sql/engine/basic/file_scan_utils.h"
static const ParseNode *find_file_node(const ParseNode *node)
{
  if (!node)
    return nullptr;
  if (node->type_ == T_TABLE_COLLECTION_EXPRESSION && node->value_ == 1)
    return node;
  for (int i = 0; i < node->num_child_; ++i)
  {
    if (const ParseNode *found = find_file_node(node->children_[i]))
      return found;
  }
  return nullptr;
}
#if defined(__linux__) && !defined(__ANDROID__)
TEST(FileSqlParser, exposes_flat_literal_arguments_to_binding)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, SMO_DEFAULT);
  ParseResult parsed;
  ASSERT_EQ(OB_SUCCESS,
            parser.parse(ObString("select * from read_csv('/tmp/input.csv',header=false,sample_size=-1) f"),
                         parsed));
  const ParseNode *node = find_file_node(parsed.result_tree_);
  ASSERT_NE(nullptr, node);
  ASSERT_EQ(3, node->children_[0]->num_child_);
  oceanbase::sql::file_sql::Options options;
  std::string path;
  ASSERT_EQ(OB_SUCCESS, oceanbase::sql::file_sql::parse_options(*node, options, path));
  EXPECT_EQ("/tmp/input.csv", path);
  EXPECT_FALSE(options.header);
}

#endif
