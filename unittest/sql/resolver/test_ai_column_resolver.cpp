/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <optional>
#include "sql/resolver/ddl/ob_ddl_resolver.h"
#include "sql/parser/ob_parser.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_operator_factory.h"
#include "sql/engine/ob_physical_plan_ctx.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;
using namespace oceanbase::share::schema;

class AiColumnResolverTest : public ::testing::Test
{
public:
  AiColumnResolverTest() : factory_(allocator_), exec_(allocator_), plan_(allocator_) {}
  static void SetUpTestCase()
  {
    ObExprOperatorFactory::register_expr_operators();
    ASSERT_EQ(OB_SUCCESS, ObPreProcessSysVars::init_sys_var());
  }
  void SetUp() override
  {
    ASSERT_EQ(OB_SUCCESS, session_.test_init(0, 1, &allocator_));
    ASSERT_EQ(OB_SUCCESS, session_.load_default_sys_variable(false, false));
    exec_.set_my_session(&session_);
    exec_.set_physical_plan_ctx(&plan_);
    exec_guard_.emplace(session_, exec_);
    params_.allocator_ = &allocator_;
    params_.session_info_ = &session_;
    params_.expr_factory_ = &factory_;
    table_.set_table_id(50000);
    table_.set_max_used_column_id(18);
    ObColumnSchemaV2 content;
    content.set_table_id(50000);
    content.set_column_id(16);
    content.set_data_type(ObVarcharType);
    content.set_data_length(200);
    content.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, content.set_column_name("content"));
    ASSERT_EQ(OB_SUCCESS, table_.add_column(content));
    result_.set_table_id(50000);
    result_.set_column_id(17);
    result_.set_data_type(ObJsonType);
    result_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, result_.set_column_name("result"));
  }
  void TearDown() override { exec_guard_.reset(); }
  int resolve(const char *expression)
  {
    int ret = result_.set_ai_expr(ObString(expression));
    return ret == OB_SUCCESS ? ObDDLResolver::resolve_ai_column_expr(params_, table_, result_) : ret;
  }
protected:
  ObArenaAllocator allocator_;
  ObRawExprFactory factory_;
  ObSQLSessionInfo session_;
  ObExecContext exec_;
  ObPhysicalPlanCtx plan_;
  std::optional<LinkExecCtxGuard> exec_guard_;
  ObResolverParams params_;
  ObTableSchema table_;
  ObColumnSchemaV2 result_;
};

TEST_F(AiColumnResolverTest, binds_dependencies_without_evaluating_model)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('any_model', concat(content, content))"));
  ObSEArray<uint64_t, 4> dependencies;
  ASSERT_EQ(OB_SUCCESS, result_.get_ai_dependencies(dependencies));
  ASSERT_EQ(1, dependencies.count());
  EXPECT_EQ(16, dependencies.at(0));
  EXPECT_NE(std::string::npos, std::string(result_.get_ai_normalized_expr().ptr(),
      result_.get_ai_normalized_expr().length()).find("Return exactly one valid JSON value"));
  EXPECT_TRUE(result_.get_cur_default_value().is_null());
  EXPECT_FALSE(result_.is_generated_column());
}

TEST_F(AiColumnResolverTest, rejects_illegal_dependencies_and_functions)
{
  EXPECT_EQ(OB_ERR_BAD_FIELD_ERROR, resolve("AI_COMPLETE('m', missing_column)"));
  EXPECT_EQ(OB_NOT_SUPPORTED, resolve("concat('x', content)"));
  EXPECT_EQ(OB_NOT_SUPPORTED, resolve("AI_COMPLETE('m', AI_COMPLETE('m', content))"));
  EXPECT_EQ(OB_NOT_SUPPORTED, resolve("AI_COMPLETE('m', concat(uuid(), content))"));
  EXPECT_EQ(OB_INVALID_ARGUMENT, resolve("AI_COMPLETE(content, content)"));
  ObColumnSchemaV2 other;
  ASSERT_EQ(OB_SUCCESS, other.assign(result_));
  other.set_column_id(18);
  ASSERT_EQ(OB_SUCCESS, other.set_column_name("other_ai"));
  ASSERT_EQ(OB_SUCCESS, table_.add_column(other));
  EXPECT_EQ(OB_NOT_SUPPORTED, resolve("AI_COMPLETE('m', other_ai)"));
}

TEST_F(AiColumnResolverTest, validates_vector_target_and_dimension)
{
  result_.set_data_type(ObCollectionSQLType);
  ASSERT_EQ(OB_SUCCESS, result_.add_type_info(ObString("VECTOR(4)")));
  EXPECT_EQ(OB_ERR_INVALID_VECTOR_DIM, resolve("AI_EMBED('m', content, 3)"));
  ASSERT_EQ(OB_SUCCESS, resolve("AI_EMBED('m', content)"));
  EXPECT_NE(std::string::npos, std::string(result_.get_ai_normalized_expr().ptr(),
      result_.get_ai_normalized_expr().length()).find(", 4)"));
  EXPECT_EQ(OB_NOT_SUPPORTED, resolve("AI_COMPLETE('m', content)"));
}

TEST_F(AiColumnResolverTest, creates_hidden_columns_without_marking_result_hidden)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m', content)"));
  ASSERT_EQ(OB_SUCCESS, ObDDLResolver::add_ai_column_hidden_columns(table_, result_));
  EXPECT_FALSE(result_.is_hidden());
  const uint64_t ids[] = {result_.get_ai_state_column_id(), result_.get_ai_request_column_id(),
                         result_.get_ai_result_spec_column_id()};
  for (uint64_t id : ids) {
    const ObColumnSchemaV2 *column = table_.get_column_schema(id);
    ASSERT_NE(nullptr, column);
    EXPECT_TRUE(column->is_hidden());
    EXPECT_FALSE(column->is_generated_column());
  }
  EXPECT_EQ(21, table_.get_max_used_column_id());
}

TEST_F(AiColumnResolverTest, prevents_schema_changes_that_break_saved_dependencies)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m', content)"));
  ASSERT_EQ(OB_SUCCESS, table_.add_column(result_));
  EXPECT_EQ(OB_NOT_SUPPORTED, ObDDLResolver::check_ai_column_dependency(table_, 16));
  EXPECT_EQ(OB_SUCCESS, ObDDLResolver::check_ai_column_dependency(table_, 18));
}

class AiColumnAttributeResolver : public ObDDLResolver
{
public:
  explicit AiColumnAttributeResolver(ObResolverParams &params) : ObDDLResolver(params) {}
  int resolve(const ParseNode &) override { return OB_NOT_SUPPORTED; }
};

static const ParseNode *find_ai_attribute(const ParseNode *node)
{
  if (node == nullptr || node->type_ == T_AI_COLUMN) { return node; }
  for (int64_t i = 0; i < node->num_child_; ++i) {
    if (const ParseNode *found = find_ai_attribute(node->children_[i])) { return found; }
  }
  return nullptr;
}

TEST_F(AiColumnResolverTest, validates_options_from_the_sql_ast)
{
  struct Case { const char *options_; int expected_; };
  const Case cases[] = {
      {"with(sync_mode='async', max_retries=0)", OB_SUCCESS},
      {"with(sync_mode='sync')", OB_NOT_SUPPORTED},
      {"with(max_retries=-1)", OB_INVALID_ARGUMENT},
      {"with(max_retries=1.5)", OB_INVALID_ARGUMENT},
      {"with(max_retries=1, max_retries=2)", OB_INVALID_ARGUMENT},
      {"with(unrecognized_option=1)", OB_INVALID_ARGUMENT}};
  for (const Case &test : cases) {
    ObSqlString sql;
    ASSERT_EQ(OB_SUCCESS, sql.append_fmt(
        "create table docs(content varchar(200), result json ai column(ai_complete('m',content)) %s)",
        test.options_));
    ObParser parser(allocator_, SMO_DEFAULT);
    ParseResult parsed;
    ASSERT_EQ(OB_SUCCESS, parser.parse(sql.string(), parsed));
    const ParseNode *attribute = find_ai_attribute(parsed.result_tree_);
    ASSERT_NE(nullptr, attribute);
    ObColumnSchemaV2 column;
    AiColumnAttributeResolver resolver(params_);
    EXPECT_EQ(test.expected_, resolver.resolve_ai_column_attribute(column, *attribute)) << test.options_;
    if (test.expected_ == OB_SUCCESS) { EXPECT_EQ(0, column.get_ai_max_retries()); }
  }
}
