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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ob_beng_ft_parser.h"
#include "data_plane/fts/ob_fts_struct.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace storage
{

namespace
{
bool is_ascii_alnum(const char ch)
{
  const unsigned char c = static_cast<unsigned char>(ch);
  const unsigned char digit_offset = static_cast<unsigned char>(c - '0');
  const unsigned char alpha_offset = static_cast<unsigned char>((c | 0x20) - 'a');
  return digit_offset <= 9 || alpha_offset <= 25;
}
}

int ObBEngFTParser::get_next_ascii_token(
    const char *&word, int64_t &word_len, int64_t &char_len, int64_t &word_freq)
{
  int ret = OB_ITER_END;
  const char *text = doc_.ptr_;
  const int64_t len = doc_.len_;
  while (ascii_pos_ < len && OB_ITER_END == ret) {
    while (ascii_pos_ < len && !is_ascii_alnum(text[ascii_pos_])) {
      ++ascii_pos_;
    }
    const int64_t begin = ascii_pos_;
    bool needs_lowercase = false;
    while (ascii_pos_ < len && is_ascii_alnum(text[ascii_pos_])) {
      const unsigned char ch = static_cast<unsigned char>(text[ascii_pos_]);
      needs_lowercase |= ch >= 'A' && ch <= 'Z';
      ++ascii_pos_;
    }
    if (begin < ascii_pos_) {
      if (needs_lowercase && nullptr == ascii_buf_
          && OB_ISNULL(ascii_buf_ = static_cast<char *>(token_allocator_->alloc(len)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (needs_lowercase) {
          for (int64_t i = begin; i < ascii_pos_; ++i) {
            ascii_buf_[i] = static_cast<char>(text[i] | 0x20);
          }
        }
        word = needs_lowercase ? ascii_buf_ + begin : text + begin;
        word_len = ascii_pos_ - begin;
        char_len = word_len;
        word_freq = 1;
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

int ObBEngFTParser::get_next_token(
    const char *&word,
    int64_t &word_len,
    int64_t &char_len,
    int64_t &word_freq)
{
  int ret = OB_SUCCESS;
  ObDatum token;
  int64_t token_freq = 0;
  char *buf = nullptr;
  word = nullptr;
  word_len = 0;
  char_len = 0;
  word_freq = 0;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (ascii_mode_) {
    ret = get_next_ascii_token(word, word_len, char_len, word_freq);
  } else if (OB_ISNULL(token_stream_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(token_stream_->get_next(token, token_freq))) {
    if (OB_ITER_END != ret) {
    }
  } else if (OB_ISNULL(token.ptr_) || OB_UNLIKELY(0 >= token.len_ || 0 >= token_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(token_allocator_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(buf = static_cast<char *>(token_allocator_->alloc(token.len_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, token.ptr_, token.len_);
    word = buf;
    word_len = token.len_;
    char_len = token.len_;
    word_freq = token_freq;
  }
  return ret;
}

int ObBEngFTParser::init(ObFTParserParam *param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(param) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < param->ft_length_)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    doc_.set_string(param->fulltext_, param->ft_length_);
    analysis_ctx_.cs_ = param->cs_;
    analysis_ctx_.filter_stopword_ = false;
    analysis_ctx_.need_grouping_ = false;
    token_allocator_ = param->allocator_;
    if (OB_FAIL(segment(doc_, token_stream_))) {
    } else if (!ascii_mode_ && OB_ISNULL(token_stream_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      is_inited_ = true;
    }
  }
  if (OB_FAIL(ret) && OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

void ObBEngFTParser::clear_document()
{
  if (nullptr != token_stream_) {
    token_stream_->reuse();
  }
  doc_.reset();
  token_allocator_ = nullptr;
  ascii_buf_ = nullptr;
  ascii_pos_ = 0;
  ascii_mode_ = false;
}

int ObBEngFTParser::reuse(ObFTParserParam *param)
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (nullptr == param || !param->is_valid() || param->cs_ != analysis_ctx_.cs_) {
    ret = OB_INVALID_ARGUMENT;
  } else if (UINT32_MAX < param->ft_length_) {
    ret = OB_NOT_SUPPORTED;
  } else {
    clear_document();
    token_allocator_ = param->allocator_;
    doc_.set_string(param->fulltext_, param->ft_length_);
    ret = segment(doc_, token_stream_);
  }
  return ret;
}

int ObBEngFTParser::segment(
    const common::ObDatum &doc,
    share::ObITokenStream *&token_stream)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(doc.ptr_) || OB_UNLIKELY(0 >= doc.len_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < doc.len_)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    const ObCharsetInfo *cs = analysis_ctx_.cs_;
    ascii_mode_ = (cs == ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN)
                   || cs == ObCharset::get_charset(CS_TYPE_UTF8MB4_GENERAL_CI));
    for (int64_t i = 0; ascii_mode_ && i < doc.len_; ++i) {
      // Invalid NUL input remains on the original multibyte tokenizer path.
      ascii_mode_ = 0 != doc.ptr_[i] && 0 == (static_cast<unsigned char>(doc.ptr_[i]) & 0x80);
    }
    ascii_pos_ = 0;
    ascii_buf_ = nullptr;
    if (!ascii_mode_ && !analyzer_inited_) {
      if (OB_FAIL(english_analyzer_.init(analysis_ctx_, allocator_))) {
      } else {
        analyzer_inited_ = true;
      }
    }
    if (OB_SUCC(ret) && !ascii_mode_ && OB_FAIL(english_analyzer_.analyze(doc, token_stream))) {
    } else if (OB_SUCC(ret) && ascii_mode_) {
      token_stream = nullptr;
    }
  }
  return ret;
}

void ObBEngFTParser::reset()
{
  analysis_ctx_.reset();
  english_analyzer_.reset();
  doc_.reset();
  token_allocator_ = nullptr;
  token_stream_ = nullptr;
  ascii_buf_ = nullptr;
  ascii_pos_ = 0;
  ascii_mode_ = false;
  analyzer_inited_ = false;
  is_inited_ = false;
}

ObBasicEnglishFTParserDesc::ObBasicEnglishFTParserDesc() {}

int ObBasicEnglishFTParserDesc::segment(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  int ret = OB_SUCCESS;
  ObBEngFTParser *parser = nullptr;
  if (OB_ISNULL(param) || OB_ISNULL(param->fulltext_) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(parser = OB_NEWx(ObBEngFTParser, param->allocator_, *(param->allocator_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser->init(param))) {
  } else {
    iter = parser;
  }

  if (OB_FAIL(ret)) {
    OB_DELETEx(ObBEngFTParser, param->allocator_, parser);
  }

  return ret;
}

void ObBasicEnglishFTParserDesc::free_token_iter(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  if (OB_NOT_NULL(iter)) {
    abort_unless(nullptr != param);
    abort_unless(nullptr != param->allocator_);
    iter->~ObITokenIterator();
    param->allocator_->free(iter);
  }
}

int ObBasicEnglishFTParserDesc::get_add_word_flag(ObAddWordFlag &flag) const
{
  int ret = OB_SUCCESS;
  flag.set_min_max_word();
  flag.set_stop_word();
  flag.set_groupby_word();
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
