/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <vector>
#include "share/schema/ob_column_schema.h"

using namespace oceanbase::common;
using namespace oceanbase::share::schema;

TEST(AiColumnSchema, owns_metadata_across_copy_serialization_and_reset)
{
  ObColumnSchemaV2 original;
  original.set_column_id(20);
  original.set_schema_version(42);
  ASSERT_EQ(OB_SUCCESS, original.set_ai_expr(ObString("AI_COMPLETE('m', content)")));
  ASSERT_EQ(OB_SUCCESS, original.set_ai_normalized_expr(ObString("normalized expression")));
  original.set_ai_max_retries(3);
  original.set_ai_definition_version(41);
  original.set_ai_state_column_id(21);
  original.set_ai_request_column_id(22);
  original.set_ai_result_spec_column_id(23);
  ObSEArray<uint64_t, 4> dependencies;
  ASSERT_EQ(OB_SUCCESS, dependencies.push_back(16));
  ASSERT_EQ(OB_SUCCESS, dependencies.push_back(19));
  ASSERT_EQ(OB_SUCCESS, original.set_ai_dependencies(dependencies));
  ObColumnSchemaV2 copied;
  ASSERT_EQ(OB_SUCCESS, copied.assign(original));
  EXPECT_NE(original.get_ai_expr().ptr(), copied.get_ai_expr().ptr());
  std::vector<char> data(original.get_serialize_size());
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, original.serialize(data.data(), data.size(), pos));
  EXPECT_EQ(data.size(), pos);
  original.reset();
  ObColumnSchemaV2 decoded;
  int64_t read_pos = 0;
  ASSERT_EQ(OB_SUCCESS, decoded.deserialize(data.data(), data.size(), read_pos));
  EXPECT_EQ(pos, read_pos);
  data.assign(data.size(), '\0');
  EXPECT_TRUE(decoded.is_ai_column());
  EXPECT_EQ(ObString("AI_COMPLETE('m', content)"), decoded.get_ai_expr());
  EXPECT_EQ(copied.get_ai_normalized_expr(), decoded.get_ai_normalized_expr());
  EXPECT_EQ(3, decoded.get_ai_max_retries());
  EXPECT_EQ(41, decoded.get_ai_definition_version());
  EXPECT_EQ(21, decoded.get_ai_state_column_id());
  EXPECT_EQ(22, decoded.get_ai_request_column_id());
  EXPECT_EQ(23, decoded.get_ai_result_spec_column_id());
  ObSEArray<uint64_t, 4> decoded_dependencies;
  ASSERT_EQ(OB_SUCCESS, decoded.get_ai_dependencies(decoded_dependencies));
  ASSERT_EQ(2, decoded_dependencies.count());
  EXPECT_EQ(16, decoded_dependencies.at(0));
  EXPECT_EQ(19, decoded_dependencies.at(1));
  decoded.reset();
  EXPECT_FALSE(decoded.is_ai_column());
  ASSERT_EQ(OB_SUCCESS, decoded.get_ai_dependencies(decoded_dependencies));
  EXPECT_EQ(0, decoded_dependencies.count());
}

TEST(AiColumnSchema, remaps_dependencies_and_hidden_column_ids)
{
  ObColumnSchemaV2 column;
  column.set_column_id(20);
  column.set_prev_column_id(19);
  column.set_next_column_id(UINT64_MAX);
  ASSERT_EQ(OB_SUCCESS, column.set_ai_expr(ObString("AI_EMBED('m', content)")));
  ObSEArray<uint64_t, 4> dependencies;
  ASSERT_EQ(OB_SUCCESS, dependencies.push_back(19));
  ASSERT_EQ(OB_SUCCESS, column.set_ai_dependencies(dependencies));
  column.set_ai_state_column_id(21);
  column.set_ai_request_column_id(22);
  column.set_ai_result_spec_column_id(23);
  hash::ObHashMap<uint64_t, uint64_t> mapping;
  ASSERT_EQ(OB_SUCCESS, mapping.create(16, "AiColumnTest"));
  for (uint64_t id = 19; id <= 23; ++id) { ASSERT_EQ(OB_SUCCESS, mapping.set_refactored(id, id + 100)); }
  ASSERT_EQ(OB_SUCCESS, column.convert_column_id(mapping));
  EXPECT_EQ(120, column.get_column_id());
  EXPECT_EQ(119, column.get_prev_column_id());
  EXPECT_EQ(121, column.get_ai_state_column_id());
  EXPECT_EQ(122, column.get_ai_request_column_id());
  EXPECT_EQ(123, column.get_ai_result_spec_column_id());
  ASSERT_EQ(OB_SUCCESS, column.get_ai_dependencies(dependencies));
  ASSERT_EQ(1, dependencies.count());
  EXPECT_EQ(119, dependencies.at(0));
}

TEST(AiColumnSchema, rejects_corrupted_dependency_metadata)
{
  ObColumnSchemaV2 column;
  ASSERT_EQ(OB_SUCCESS, column.set_ai_dependency_ids(ObString("bad data")));
  ObSEArray<uint64_t, 4> dependencies;
  EXPECT_NE(OB_SUCCESS, column.get_ai_dependencies(dependencies));
}
