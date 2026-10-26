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
#include "sql/engine/expr/ob_expr_ai/ob_expr_ai_embed.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

class TestAIEmbed : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;
  ObIJsonBase *parse(const char *text)
  {
    ObIJsonBase *result = nullptr;
    ObString input(text);
    EXPECT_EQ(OB_SUCCESS, ObJsonBaseFactory::get_json_base(&allocator_, input,
        ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, result));
    return result;
  }
};

TEST_F(TestAIEmbed, restores_index_order)
{
  auto data = static_cast<ObJsonNode *>(parse(R"([{"index":2,"embedding":[30,31]},{"index":0,"embedding":[10,11]},{"index":1,"embedding":[20,21]}])"));
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::parse_embedding_array(allocator_, data, "index", result));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_, result, 3, 2));
  ObString text;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(allocator_, result, text));
  EXPECT_EQ(ObString("[[10, 11], [20, 21], [30, 31]]"), text);
}

TEST_F(TestAIEmbed, rejects_invalid_responses)
{
  const char *invalid[] = {"{}", "null", "[]", "[[]]", "[[1, null]]", "[[1, true]]",
      "[[1, \"2\"]]", "[[1, 2], [3]]", "[1]"};
  for (const char *text : invalid) {
    EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_, parse(text), 1, 2)) << text;
  }
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_, parse("[[1,2]]"), 1, 0));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_, parse("[[1,2]]"), 1, 3));
  const char *indexes[] = {
      R"([{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}])",
      R"([{"index":-1,"embedding":[1]}])", R"([{"index":1,"embedding":[1]}])",
      R"([{"index":0.5,"embedding":[1]}])", R"([{"index":"0","embedding":[1]}])",
      R"([{"index":0,"embedding":[1]},{"embedding":[2]}])"};
  for (const char *text : indexes) {
    ObIJsonBase *result = nullptr;
    EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::parse_embedding_array(allocator_,
        static_cast<ObJsonNode *>(parse(text)), "index", result)) << text;
    EXPECT_EQ(nullptr, result);
  }
  ObJsonArray *vectors = nullptr;
  ObJsonArray *vector = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, vectors));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, vector));
  auto infinity = OB_NEWx(ObJsonDouble, &allocator_, std::numeric_limits<double>::infinity());
  ASSERT_NE(nullptr, infinity);
  ASSERT_EQ(OB_SUCCESS, vector->append(infinity));
  ASSERT_EQ(OB_SUCCESS, vectors->append(vector));
  EXPECT_NE(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_, vectors, 1, 1));
}

TEST_F(TestAIEmbed, request_contains_multiple_inputs_and_dimension)
{
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObString model("embedding-model");
  ObArray<ObString> inputs;
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("A")));
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("B")));
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(ObString("C")));
  ObJsonObject *config = nullptr;
  ObJsonInt *dim = nullptr;
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, config));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_int(allocator_, 2, dim));
  ASSERT_EQ(OB_SUCCESS, config->add("dimensions", dim));
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator_, model, inputs, config, body));
  ASSERT_NE(nullptr, body->get_value("input"));
  EXPECT_EQ(3, body->get_value("input")->element_count());
  EXPECT_EQ(2, body->get_value("dimensions")->get_int());
}

TEST_F(TestAIEmbed, rejects_fractional_dimension_types)
{
  ObExprAIEmbed function(allocator_);
  ObExprResType args[3];
  args[0].set_varchar();
  args[1].set_type(ObLongTextType);
  ObExprResType result;
  ObExprTypeCtx ctx;
  args[2].set_double();
  EXPECT_EQ(OB_INVALID_ARGUMENT, function.calc_result_typeN(result, args, 3, ctx));
  args[2].set_type(ObUInt64Type);
  EXPECT_EQ(OB_SUCCESS, function.calc_result_typeN(result, args, 3, ctx));
  EXPECT_EQ(ObUInt64Type, args[2].get_calc_type());
  EXPECT_EQ(ObLongTextType, args[1].get_calc_type());
}

TEST_F(TestAIEmbed, dimension_value_boundaries)
{
  ObDatum datum;
  char buffer[16];
  datum.ptr_ = buffer;
  int64_t dimension = 0;
  datum.set_int(768);
  EXPECT_EQ(OB_SUCCESS, ObExprAIEmbed::get_dimension(datum, ObIntType, dimension));
  EXPECT_EQ(768, dimension);
  datum.set_int(0);
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObExprAIEmbed::get_dimension(datum, ObIntType, dimension));
  datum.set_int(-1);
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObExprAIEmbed::get_dimension(datum, ObIntType, dimension));
  datum.set_uint(UINT64_MAX);
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObExprAIEmbed::get_dimension(datum, ObUInt64Type, dimension));
  datum.set_null();
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObExprAIEmbed::get_dimension(datum, ObIntType, dimension));
  datum.set_double(1.5);
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObExprAIEmbed::get_dimension(datum, ObDoubleType, dimension));
}

TEST_F(TestAIEmbed, batch_preserves_skip_and_evaluated_rows)
{
  ObExecContext exec(allocator_);
  ObEvalCtx ctx(exec);
  ctx.set_max_batch_size(4);
  char *frames[3];
  ObExpr output, model, content;
  ObExpr *args[] = {&model, &content};
  ObExpr *expressions[] = {&output, &model, &content};
  for (int64_t i = 0; i < 3; ++i) {
    frames[i] = static_cast<char *>(allocator_.alloc(512));
    ASSERT_NE(nullptr, frames[i]);
    MEMSET(frames[i], 0, 512);
    expressions[i]->frame_idx_ = i;
    expressions[i]->datum_off_ = 0;
    expressions[i]->eval_info_off_ = 128;
    expressions[i]->eval_flags_off_ = 192;
    expressions[i]->res_buf_off_ = 256;
    for (int64_t row = 0; row < 4; ++row) {
      new (frames[i] + row * sizeof(ObDatum)) ObDatum();
    }
  }
  ctx.frames_ = frames;
  output.batch_result_ = true;
  output.args_ = args;
  output.arg_cnt_ = 2;
  content.batch_result_ = true;
  model.locate_expr_datum(ctx).set_string("model");
  for (int64_t i = 0; i < 4; ++i) {
    content.locate_batch_datums(ctx)[i].set_null();
    output.locate_batch_datums(ctx)[i].set_string("keep");
  }
  auto &flags = output.get_evaluated_flags(ctx);
  flags.reset(4);
  flags.set(1);
  void *bits = allocator_.alloc(ObBitVector::memory_size(4));
  ASSERT_NE(nullptr, bits);
  auto skip = to_bit_vector(bits);
  skip->reset(4);
  skip->set(0);
  ASSERT_EQ(OB_SUCCESS, ObExprAIEmbed::eval_ai_embed_batch(output, ctx, *skip, 4));
  EXPECT_EQ(ObString("keep"), output.locate_batch_datums(ctx)[0].get_string());
  EXPECT_EQ(ObString("keep"), output.locate_batch_datums(ctx)[1].get_string());
  EXPECT_TRUE(output.locate_batch_datums(ctx)[2].is_null());
  EXPECT_TRUE(output.locate_batch_datums(ctx)[3].is_null());
  EXPECT_FALSE(flags.at(0));
  EXPECT_TRUE(flags.at(1));
  EXPECT_TRUE(flags.at(2));
  EXPECT_TRUE(flags.at(3));
  // A second invocation has no pending rows and must not evaluate arguments.
  content.eval_batch_func_ = [](const ObExpr &, ObEvalCtx &, const ObBitVector &, int64_t) {
    return OB_ERR_UNEXPECTED;
  };
  ASSERT_EQ(OB_SUCCESS, ObExprAIEmbed::eval_ai_embed_batch(output, ctx, *skip, 4));
}
