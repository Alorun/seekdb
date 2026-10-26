/*
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

#include <gtest/gtest.h>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

class TestAIRerank : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;
  ObIJsonBase *parse(const char *json)
  {
    ObString text(json);
    ObIJsonBase *result = nullptr;
    EXPECT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator_, text,
        ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, result));
    return result;
  }
};

TEST_F(TestAIRerank, unordered_scores_and_stable_ties)
{
  auto input = parse(R"([{"index":2,"relevance_score":0.5},{"index":0,"relevance_score":0.5},{"index":1,"relevance_score":1}])");
  ObJsonArray *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::sort_rerank_result(allocator_, input, 3, result));
  ASSERT_EQ(3, result->element_count());
  EXPECT_EQ(1, static_cast<ObJsonObject *>(result->get_value(0))->get_value("index")->get_int());
  EXPECT_EQ(0, static_cast<ObJsonObject *>(result->get_value(1))->get_value("index")->get_int());
  EXPECT_EQ(2, static_cast<ObJsonObject *>(result->get_value(2))->get_value("index")->get_int());
}

TEST_F(TestAIRerank, rejects_missing_duplicate_and_invalid_fields)
{
  const char *invalid[] = {"{}", "[]", "[1]", R"([{"index":-1,"relevance_score":0.5}])",
      R"([{"index":1,"relevance_score":0.5}])", R"([{"index":0.5,"relevance_score":0.5}])",
      R"([{"index":"0","relevance_score":0.5}])", R"([{"index":0,"relevance_score":"0.5"}])",
      R"([{"index":0}])", R"([{"relevance_score":0.5}])"};
  for (const char *text : invalid) {
    ObJsonArray *result = nullptr;
    EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::sort_rerank_result(allocator_, parse(text), 1, result)) << text;
    EXPECT_EQ(nullptr, result);
  }
  ObJsonArray *result = nullptr;
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::sort_rerank_result(allocator_,
      parse(R"([{"index":0,"relevance_score":1},{"index":0,"relevance_score":2}])"), 2, result));
}
