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

#include "object/ob_object.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "share/rc/ob_server_runtime.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/ob_fts_parser_property.h"

namespace oceanbase
{
namespace storage
{

namespace
{
bool is_ascii_lower_token(const ObString &word)
{
  bool is_ascii_lower = !word.empty();
  for (int64_t i = 0; is_ascii_lower && i < word.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(word.ptr()[i]);
    is_ascii_lower = c < 0x80 && !(c >= 'A' && c <= 'Z');
  }
  return is_ascii_lower;
}

bool is_ascii_token(const ObString &word)
{
  bool is_ascii = true;
  for (int64_t i = 0; is_ascii && i < word.length(); ++i) {
    is_ascii = static_cast<unsigned char>(word.ptr()[i]) < 0x80;
  }
  return is_ascii;
}

bool is_builtin_ascii_stopword(const ObString &word)
{
  bool is_stopword = false;
  const char *text = word.ptr();
  switch (word.length()) {
  case 1:
    is_stopword = 'a' == text[0] || 'i' == text[0];
    break;
  case 2:
    switch (text[0]) {
    case 'a': is_stopword = 'n' == text[1] || 's' == text[1] || 't' == text[1]; break;
    case 'b': is_stopword = 'e' == text[1] || 'y' == text[1]; break;
    case 'd': is_stopword = 'e' == text[1]; break;
    case 'e': is_stopword = 'n' == text[1]; break;
    case 'i': is_stopword = 'n' == text[1] || 's' == text[1] || 't' == text[1]; break;
    case 'l': is_stopword = 'a' == text[1]; break;
    case 'o': is_stopword = 'f' == text[1] || 'n' == text[1] || 'r' == text[1]; break;
    case 't': is_stopword = 'o' == text[1]; break;
    default: break;
    }
    break;
  case 3:
    switch (text[0]) {
    case 'a': is_stopword = 0 == MEMCMP(text, "are", 3); break;
    case 'c': is_stopword = 0 == MEMCMP(text, "com", 3); break;
    case 'f': is_stopword = 0 == MEMCMP(text, "for", 3); break;
    case 'h': is_stopword = 0 == MEMCMP(text, "how", 3); break;
    case 't': is_stopword = 0 == MEMCMP(text, "the", 3); break;
    case 'u': is_stopword = 0 == MEMCMP(text, "und", 3); break;
    case 'w': is_stopword = 0 == MEMCMP(text, "was", 3)
                              || 0 == MEMCMP(text, "who", 3)
                              || 0 == MEMCMP(text, "www", 3); break;
    default: break;
    }
    break;
  case 4:
    switch (text[0]) {
    case 'f': is_stopword = 0 == MEMCMP(text, "from", 4); break;
    case 't': is_stopword = 0 == MEMCMP(text, "that", 4)
                              || 0 == MEMCMP(text, "this", 4); break;
    case 'w': is_stopword = 0 == MEMCMP(text, "what", 4)
                              || 0 == MEMCMP(text, "when", 4)
                              || 0 == MEMCMP(text, "will", 4)
                              || 0 == MEMCMP(text, "with", 4); break;
    default: break;
    }
    break;
  case 5:
    is_stopword = 0 == MEMCMP(text, "about", 5)
                  || 0 == MEMCMP(text, "where", 5);
    break;
  default:
    break;
  }
  return is_stopword;
}

static constexpr int64_t MAX_BUILTIN_ASCII_STOPWORD_LENGTH = 5;
}

////////////////////////////////////////////////////////////////////////////////
// class ObStopWordChecker
ObStopWordChecker::~ObStopWordChecker()
{
  destroy();
}

int ObStopWordChecker::init()
{
  int ret = OB_SUCCESS;
  
  

  if (inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(stopword_set_.create(DEFAULT_STOPWORD_BUCKET_NUM, "StopWordSet", "StopWordSet"))) {
  } else {
    ObObjMeta stop_meta;
    stop_meta.set_varchar();
    stop_meta.set_collation_type(ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI);

    stopword_type_.set_meta(stop_meta);
    const int64_t stopword_count = sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]);
    for (int64_t i = 0; OB_SUCC(ret) && i < stopword_count; ++i) {
      ObFTWord stopword(STRLEN(ob_stop_word_list[i]), ob_stop_word_list[i], stopword_type_);
      if (OB_FAIL(stopword_set_.set_refactored(stopword))) {
      }
    }

    if (OB_SUCC(ret)) {
      inited_ = true;
    }
  }
  return ret;
}

void ObStopWordChecker::destroy()
{
  if (inited_) {
    stopword_set_.destroy();
    inited_ = false;
  }
}

int ObStopWordChecker::check_stopword(const ObFTWord &word,
                                      bool &is_stopword,
                                      const bool is_ascii_lowercase)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const ObString source = word.get_word().get_string();
    if (is_ascii_lowercase && source.length() > MAX_BUILTIN_ASCII_STOPWORD_LENGTH) {
      is_stopword = false;
    } else if (is_ascii_lowercase) {
      is_stopword = is_builtin_ascii_stopword(source);
    } else {
      ObFTWord converted;
      const ObFTWord *lookup_word = &word;
      if (word.get_collation_type() == stopword_type_.get_collation_type()) {
        // The hash key already uses the stopword collation.
      } else if (ObCharset::get_charset(word.get_collation_type())
                     == ObCharset::get_charset(stopword_type_.get_collation_type())
                 && is_ascii_token(source)) {
        // ASCII bytes have identical code points across collations of one charset.
        converted = ObFTWord(source.length(), source.ptr(), stopword_type_);
        lookup_word = &converted;
      } else {
        common::ObArenaAllocator allocator(lib::ObMemAttr("ChkStopWord"));
        common::ObString cmp_str;
        if (OB_FAIL(common::ObCharset::charset_convert(
                        allocator,
                        source,
                        word.get_collation_type(),
                        stopword_type_.get_collation_type(),
                        cmp_str))) {
        } else {
          converted = ObFTWord(cmp_str.length(), cmp_str.ptr(), stopword_type_);
          lookup_word = &converted;
        }
      }
      if (OB_SUCC(ret)) {
        ret = stopword_set_.exist_refactored(*lookup_word);
        if (OB_HASH_NOT_EXIST == ret) {
          is_stopword = false;
          ret = OB_SUCCESS;
        } else if (OB_HASH_EXIST == ret) {
          is_stopword = true;
          ret = OB_SUCCESS;
        } else if (OB_SUCC(ret)) {
          ret = OB_ERR_UNEXPECTED;
        }
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// class ObAddWord
ObAddWord::ObAddWord(
    const ObFTParserProperty &property,
    const ObObjMeta &meta,
    const ObAddWordFlag &flag,
    common::ObIAllocator &allocator,
    ObFTWordMap &word_map)
  : word_meta_(meta),
    allocator_(allocator),
    word_map_(word_map),
    min_max_word_cnt_(0),
    non_stopword_cnt_(0),
    stopword_cnt_(0),
    min_token_size_(property.min_token_size_),
    max_token_size_(property.max_token_size_),
    flag_(flag)
{
}

int ObAddWord::process_word(
    const char *word,
    const int64_t word_len,
    const int64_t char_cnt,
    const int64_t word_freq,
    const bool is_ascii_lowercase)
{
  int ret = OB_SUCCESS;
  bool is_stopword = false;
  ObFTWord src_word(word_len, word, word_meta_);
  ObFTWord dst_word;
  if (OB_ISNULL(word) || OB_UNLIKELY(0 >= word_len || 0 >= char_cnt || 0 >= word_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (is_min_max_word(char_cnt)) {
    ++min_max_word_cnt_;
  } else if (OB_FAIL(casedown_word(src_word, dst_word, is_ascii_lowercase))) {
  } else if (OB_FAIL(check_stopword(dst_word, is_stopword, is_ascii_lowercase))) {
  } else if (OB_UNLIKELY(is_stopword)) {
    ++stopword_cnt_;
  } else if (OB_FAIL(groupby_word(dst_word, word_freq, is_ascii_lowercase))) {
  } else {
    non_stopword_cnt_ += word_freq;
  }
  return ret;
}

bool ObAddWord::is_min_max_word(const int64_t c_len) const
{
  return flag_.min_max_word() && (c_len < min_token_size_ || c_len > max_token_size_);
}

int ObAddWord::casedown_word(const ObFTWord &src,
                             ObFTWord &dst,
                             const bool is_ascii_lowercase)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.casedown()) {
    const ObString source = src.get_word().get_string();
    const ObCollationType collation = word_meta_.get_collation_type();
    const bool utf8mb4_ascii = (collation == CS_TYPE_UTF8MB4_BIN
                                || collation == CS_TYPE_UTF8MB4_GENERAL_CI)
                               && (is_ascii_lowercase || is_ascii_lower_token(source));
    if (utf8mb4_ascii) {
      dst = src;
    } else {
      ObString dst_str;
      if (OB_FAIL(ObCharset::tolower(collation, source, dst_str, allocator_))) {
      } else {
        ObFTWord tmp(dst_str.length(), dst_str.ptr(), word_meta_);
        dst = tmp;
      }
    }
  } else {
    dst = src;
  }
  return ret;
}

int ObAddWord::check_stopword(const ObFTWord &ft_word,
                              bool &is_stopword,
                              const bool is_ascii_lowercase)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ft_word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.stopword()) {
    ObStopWordChecker *stop_word_checker = ObFTParseData::instance().stop_word_checker();
    if (OB_ISNULL(stop_word_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(stop_word_checker->check_stopword(ft_word, is_stopword,
                                                        is_ascii_lowercase))) {
    }
  }
  return ret;
}

int ObAddWord::groupby_word(const ObFTWord &word,
                            const int64_t word_freq,
                            const bool is_ascii_lowercase)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(word.empty() || word_freq <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!flag_.groupby_word()) {
    if (OB_FAIL(word_map_.set_refactored(word, 1/*word count*/))) {
    }
  } else {
    // ASCII fast-path words already have their canonical lowercase spelling.
    // Other collations retain the prior key replacement behavior.
    auto add_frequency = [&word, word_freq, is_ascii_lowercase](
        common::hash::HashMapPair<ObFTWord, int64_t> &entry) {
      if (!is_ascii_lowercase) {
        entry.first = word;
      }
      entry.second += word_freq;
    };
    ret = word_map_.set_or_update(word, 1/*initial word count*/, add_frequency);
  }
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
