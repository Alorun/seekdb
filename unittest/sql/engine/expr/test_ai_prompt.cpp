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
#include "sql/engine/expr/ob_expr_ai/ob_expr_ai_prompt.h"

using namespace oceanbase::common;

class TestAIPrompt : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;

  int render(const char *json, ObString &result)
  {
    ObJsonObject *object = nullptr;
    ObString input(json);
    int ret = ObAIFuncJsonUtils::get_json_object_form_str(allocator_, input, object);
    if (OB_SUCC(ret)) {
      ret = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, object, result);
    }
    return ret;
  }
};

TEST_F(TestAIPrompt, repeated_long_argument)
{
  ObJsonObject *object = nullptr;
  ObJsonArray *args = nullptr;
  ObJsonString *value = nullptr;
  char text[8192];
  MEMSET(text, 'x', sizeof(text));
  ObString argument(sizeof(text), text);
  ObString format("{0}/{0}/{0}");
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, args));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator_, argument, value));
  ASSERT_EQ(OB_SUCCESS, args->append(value));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(allocator_, format, args, object));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, object, result));
  ASSERT_EQ(3 * sizeof(text) + 2, result.length());
  EXPECT_EQ('/', result.ptr()[sizeof(text)]);
  EXPECT_EQ(0, MEMCMP(text, result.ptr() + sizeof(text) + 1, sizeof(text)));
}

TEST_F(TestAIPrompt, json_and_unicode_escaping)
{
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(R"({"template":"{0}/{0}/{1}","args":[[1,true],{"k":"你好\"\\"}]})", result));
  EXPECT_EQ(ObString("[1, true]/[1, true]/{\"k\": \"你好\\\"\\\\\"}"), result);
}

TEST_F(TestAIPrompt, placeholder_boundaries)
{
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(R"({"template":"{} {x} {-1} {0 {00}","args":["ok"]})", result));
  EXPECT_EQ(ObString("{} {x} {-1} {0 ok"), result);
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(R"({"template":"{1}","args":["ok"]})", result));
  EXPECT_TRUE(result.empty());
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(R"({"template":"{184467440737095516160000}","args":["ok"]})", result));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(R"({"template":42,"args":[]})", result));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(R"({"template":"{0}","args":[null]})", result));
}

TEST(TestAIPromptTypes, preserves_lob_and_json)
{
  ObArenaAllocator allocator;
  oceanbase::sql::ObExprAIPrompt function(allocator);
  oceanbase::sql::ObExprResType types[3];
  oceanbase::sql::ObExprResType result;
  ObExprTypeCtx context;
  types[0].set_type(ObLongTextType);
  types[1].set_type(ObTextType);
  types[2].set_json();
  ASSERT_EQ(OB_SUCCESS, function.calc_result_typeN(result, types, 3, context));
  EXPECT_EQ(ObLongTextType, types[0].get_calc_type());
  EXPECT_EQ(ObTextType, types[1].get_calc_type());
  EXPECT_EQ(ObJsonType, types[2].get_calc_type());
  types[1].set_null();
  EXPECT_EQ(OB_ERR_INVALID_TYPE_FOR_OP, function.calc_result_typeN(result, types, 3, context));
}

TEST_F(TestAIPrompt, nested_prompt_value)
{
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(
      R"({"template":"outer:{0}/{0}","args":[{"template":"inner:{0}","args":["你好"]}]})", result));
  EXPECT_EQ(ObString("outer:inner:你好/inner:你好"), result);
}
