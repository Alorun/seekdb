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

class TestAIProviders : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;
  ObJsonObject *object(const char *text)
  {
    ObString input(text);
    ObJsonObject *result = nullptr;
    EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, input, result));
    return result;
  }
};

TEST_F(TestAIProviders, openai_completion_request_and_config)
{
  ObOpenAIUtils::ObOpenAIComplete provider;
  ObString model("request-model"), system, prompt("你好\"\\\n");
  ObJsonObject *body = nullptr;
  auto config = object(R"({"temperature":0.2,"max_tokens":16})");
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator_, model, system, prompt, config, body));
  EXPECT_EQ(model, static_cast<ObJsonString *>(body->get_value("model"))->get_str());
  EXPECT_EQ(16, body->get_value("max_tokens")->get_int());
  auto messages = static_cast<ObJsonArray *>(body->get_value("messages"));
  ASSERT_EQ(1, messages->element_count());
  auto message = static_cast<ObJsonObject *>(messages->get_value(0));
  EXPECT_EQ(prompt, static_cast<ObJsonString *>(message->get_value("content"))->get_str());
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator_, model, system, prompt, nullptr, body));
  const char *invalid[] = {R"({"temperature":"0.2"})", R"({"max_tokens":0})",
      R"({"max_tokens":-1})", R"({"max_tokens":1.5})", R"({"max_tokens":null})"};
  for (const char *text : invalid) {
    EXPECT_NE(OB_SUCCESS, provider.get_body(allocator_, model, system, prompt, object(text), body));
  }
}

TEST_F(TestAIProviders, completion_response_requires_nonempty_string)
{
  ObOpenAIUtils::ObOpenAIComplete openai;
  ObOllamaUtils::ObOllamaComplete ollama;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, openai.parse_output(allocator_, object(R"({"choices":[{"message":{"content":"ok"}}]})"), result));
  const char *invalid[] = {"{}", R"({"choices":[]})", R"({"choices":[{"message":{"content":""}}]})",
      R"({"choices":[{"message":{"content":42}}]})", R"({"choices":[{"message":{"content":null}}]})"};
  for (const char *text : invalid) {
    EXPECT_NE(OB_SUCCESS, openai.parse_output(allocator_, object(text), result));
  }
  EXPECT_EQ(OB_SUCCESS, ollama.parse_output(allocator_, object(R"({"response":"ok","done":true})"), result));
  EXPECT_NE(OB_SUCCESS, ollama.parse_output(allocator_, object(R"({"response":""})"), result));
  EXPECT_NE(OB_SUCCESS, ollama.parse_output(allocator_, object(R"({"response":[]})"), result));
}

TEST_F(TestAIProviders, ollama_dispatch_and_native_protocol)
{
  ObAIFuncIComplete *complete = nullptr;
  ObAIFuncIEmbed *embed = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator_, ObString("Ollama"), complete));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_embed_provider(allocator_, ObString("oLLama"), embed));
  ObString key, model("model"), system, prompt("hello");
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, complete->get_header(allocator_, key, headers));
  ASSERT_EQ(1, headers.count());
  EXPECT_EQ(ObString("Content-Type: application/json"), headers.at(0));
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete->get_body(allocator_, model, system, prompt, nullptr, body));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  EXPECT_EQ(nullptr, body->get_value("messages"));
  EXPECT_EQ(prompt, static_cast<ObJsonString *>(body->get_value("prompt"))->get_str());
  ObArray<ObString> inputs;
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(prompt));
  ASSERT_EQ(OB_SUCCESS, embed->get_body(allocator_, model, inputs, nullptr, body));
  EXPECT_EQ(1, body->get_value("input")->element_count());
  ObIJsonBase *vectors = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed->parse_output(allocator_, object(R"({"embeddings":[[1,2]]})"), vectors));
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_, vectors, 1, 2));
  ObOpenAIUtils::ObOpenAIComplete openai;
  EXPECT_NE(OB_SUCCESS, openai.get_header(allocator_, key, headers));
}

TEST_F(TestAIProviders, dashscope_response_validation)
{
  ObDashscopeUtils::ObDashscopeComplete complete;
  ObDashscopeUtils::ObDashscopeEmbed embed;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete.parse_output(allocator_,
      object(R"({"output":{"choices":[{"message":{"content":"ok"}}]}})"), result));
  EXPECT_NE(OB_SUCCESS, complete.parse_output(allocator_, object(R"({"output":[]})"), result));
  EXPECT_NE(OB_SUCCESS, complete.parse_output(allocator_, object(R"({"output":{"choices":[]}})"), result));
  EXPECT_NE(OB_SUCCESS, embed.parse_output(allocator_, object(R"({"output":[]})"), result));
  ASSERT_EQ(OB_SUCCESS, embed.parse_output(allocator_, object(
      R"({"output":{"embeddings":[{"text_index":1,"embedding":[3,4]},{"text_index":0,"embedding":[1,2]}]}})"), result));
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::validate_embeddings(allocator_, result, 2, 2));
}
