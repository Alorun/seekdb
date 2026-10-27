/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "sql/parser/ob_parser.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

static const ParseNode *find_ai_column(const ParseNode *node)
{
  if (node == nullptr || node->type_ == T_AI_COLUMN) { return node; }
  for (int64_t i = 0; i < node->num_child_; ++i) {
    if (const ParseNode *found = find_ai_column(node->children_[i])) { return found; }
  }
  return nullptr;
}

TEST(AiColumnParser, preserves_expression_and_options_as_ast)
{
  ObArenaAllocator allocator;
  ObParser parser(allocator, SMO_DEFAULT);
  ParseResult parsed;
  ASSERT_EQ(OB_SUCCESS, parser.parse(ObString(
      "create table docs(id bigint primary key, content varchar(200), "
      "tags json AI COLUMN (AI_COMPLETE('model', concat('Classify: ', content))) "
      "WITH (sync_mode='async', max_retries=2))"), parsed)) << parsed.error_msg_ << " at " << parsed.start_col_;
  const ParseNode *ai = find_ai_column(parsed.result_tree_);
  ASSERT_NE(nullptr, ai);
  ASSERT_EQ(2, ai->num_child_);
  ASSERT_NE(nullptr, ai->children_[0]);
  EXPECT_EQ("AI_COMPLETE('model', concat('Classify: ', content))",
      std::string(ai->children_[0]->str_value_, ai->children_[0]->str_len_));
  ASSERT_NE(nullptr, ai->children_[1]);
  EXPECT_EQ(T_AI_COLUMN_OPTIONS, ai->children_[1]->type_);
  EXPECT_EQ(2, ai->children_[1]->num_child_);
}

TEST(AiColumnParser, accepts_create_alter_and_ordinary_sql)
{
  const char *queries[] = {
      "create table docs(content varchar(200), emb vector(4) ai column (ai_embed('m', content)))",
      "alter table docs add column tags json ai column (ai_complete('m', content)) with(max_retries=0)",
      "alter table docs modify tags json ai column (ai_complete('m', content)) with(sync_mode='async')",
      "create table docs(ai int, content varchar(200), g varchar(200) generated always as (upper(content)) stored)",
      "select ai_complete('m', concat('prefix', content)), ai_embed('e', content, 4) from docs"};
  for (const char *query : queries) {
    ObArenaAllocator allocator;
    ObParser parser(allocator, SMO_DEFAULT);
    ParseResult parsed;
    EXPECT_EQ(OB_SUCCESS, parser.parse(ObString(query), parsed)) << query << " " << parsed.error_msg_ << " at " << parsed.start_col_;
  }
}

TEST(AiColumnParser, rejects_incomplete_and_malformed_declarations)
{
  const char *queries[] = {
      "create table docs(tags json ai column ())",
      "create table docs(tags json ai (ai_complete('m', 'x')))",
      "create table docs(tags json ai column (ai_complete('m', 'x')) with())",
      "create table docs(tags json ai column (ai_complete('m', 'x')) with(max_retries=))",
      "create table docs(tags json ai column (ai_complete('m', 'x')) with(max_retries=1,))"};
  for (const char *query : queries) {
    ObArenaAllocator allocator;
    ObParser parser(allocator, SMO_DEFAULT);
    ParseResult parsed;
    EXPECT_NE(OB_SUCCESS, parser.parse(ObString(query), parsed)) << query << " " << parsed.error_msg_ << " at " << parsed.start_col_;
  }
}
