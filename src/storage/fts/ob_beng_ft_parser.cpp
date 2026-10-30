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
bool is_ascii_delimiter(const ObCharsetInfo *cs, const char ch)
{
  return ob_isspace(cs, ch) || ob_iscntrl(cs, ch) || ob_ispunct(cs, ch);
}
}

int ObBEngFTParser::get_next_ascii_token(
    const char *&word, int64_t &word_len, int64_t &char_len, int64_t &word_freq)
{
  int ret = OB_ITER_END;
  if (ascii_token_idx_ < ascii_buffer_pos_) {
    const uint32_t begin = ascii_token_idx_;
    uint32_t end = begin;
    while (end < ascii_buffer_pos_ && 0 != ascii_buf_[end]) {
      ++end;
    }
    word = ascii_buf_ + begin;
    word_len = end - begin;
    char_len = word_len;
    word_freq = 1;
    ascii_token_idx_ = end >= ascii_buffer_pos_ ? ascii_buffer_pos_ : end + 1;
    ret = OB_SUCCESS;
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
    LOG_DEBUG("succeed to add word", K(ObString(word_len, word)), K(word_freq));
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
    if (OB_FAIL(english_analyzer_.init(analysis_ctx_, allocator_))) {
    } else if (OB_FAIL(segment(doc_, token_stream_))) {
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

void ObBEngFTParser::clear_document(const bool release_buffer)
{
  if (nullptr != token_stream_) {
    token_stream_->reuse();
  }
  doc_.reset();
  token_allocator_ = nullptr;
  // Keep the document-scoped ASCII buffer while the arena remains alive.  A
  // caller that resets that arena passes release_buffer=true so the pointer
  // cannot outlive its allocation.
  if (release_buffer) {
    ascii_buf_ = nullptr;
    ascii_buf_allocator_ = nullptr;
    ascii_buf_capacity_ = 0;
  }
  ascii_token_idx_ = 0;
  ascii_buffer_pos_ = 0;
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
    if (ascii_mode_ && ascii_table_cs_ != cs) {
      static const uint8_t DELIMITER = 1;
      static const uint8_t ALNUM = 2;
      for (int64_t i = 0; i < 128; ++i) {
        const char c = static_cast<char>(i);
        ascii_class_[i] = 0;
        if (is_ascii_delimiter(cs, c)) {
          ascii_class_[i] |= DELIMITER;
        }
        if (ob_isalnum(cs, c)) {
          ascii_class_[i] |= ALNUM;
        }
        ascii_lower_[i] = ob_tolower(cs, c);
      }
      ascii_table_cs_ = cs;
    }
    ascii_token_idx_ = 0;
    ascii_buffer_pos_ = 0;
    if (ascii_mode_) {
      static const uint8_t DELIMITER = 1;
      static const uint8_t ALNUM = 2;
      uint32_t token_count = 0;
      int64_t token_start = -1;
      int64_t last_alnum = -1;
      for (int64_t i = 0; OB_SUCC(ret) && ascii_mode_ && i < doc.len_; ++i) {
        const uint8_t c = static_cast<uint8_t>(doc.ptr_[i]);
        if (0 == c || 0 != (c & 0x80)) {
          ascii_mode_ = false;
        } else if ((ascii_class_[c] & DELIMITER) != 0) {
          if (token_start >= 0) {
            ascii_buffer_pos_ = last_alnum + 1;
            ++token_count;
            token_start = -1;
            last_alnum = -1;
          }
        } else {
          if (token_start < 0 && (ascii_class_[c] & ALNUM) != 0) {
            if (OB_ISNULL(token_allocator_)) {
              ret = OB_ERR_UNEXPECTED;
            } else if (ascii_buf_allocator_ != token_allocator_
                       || ascii_buf_capacity_ < doc.len_
                       || nullptr == ascii_buf_) {
              char *new_buf = static_cast<char *>(token_allocator_->alloc(doc.len_));
              if (OB_ISNULL(new_buf)) {
                ret = OB_ALLOCATE_MEMORY_FAILED;
              } else {
                ascii_buf_ = new_buf;
                ascii_buf_allocator_ = token_allocator_;
                ascii_buf_capacity_ = doc.len_;
              }
            }
            if (OB_SUCC(ret)) {
              if (token_count > 0) {
                ascii_buf_[ascii_buffer_pos_++] = '\0';
              }
              token_start = ascii_buffer_pos_;
            }
          }
          if (OB_SUCC(ret) && token_start >= 0) {
            ascii_buf_[ascii_buffer_pos_] = ascii_lower_[c];
            if ((ascii_class_[c] & ALNUM) != 0) {
              last_alnum = ascii_buffer_pos_;
            }
            ++ascii_buffer_pos_;
          }
        }
      }
      if (OB_SUCC(ret) && ascii_mode_ && token_start >= 0) {
        ascii_buffer_pos_ = last_alnum + 1;
      }
      if (!ascii_mode_) {
        ascii_token_idx_ = 0;
        ascii_buffer_pos_ = 0;
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
  ascii_buf_allocator_ = nullptr;
  ascii_buf_capacity_ = 0;
  ascii_token_idx_ = 0;
  ascii_buffer_pos_ = 0;
  ascii_mode_ = false;
  ascii_table_cs_ = nullptr;
  MEMSET(ascii_class_, 0, sizeof(ascii_class_));
  MEMSET(ascii_lower_, 0, sizeof(ascii_lower_));
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
