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
#include "storage/fts/ob_fts_struct.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ob_fts_parser_helper.h"

#include "common/json_type/ob_json_tree.h"
#include "share/ob_force_print_log.h"
#include "storage/fts/dict/ob_ft_dict_hub.h"
#include "storage/fts/ob_beng_ft_parser.h"
#include "storage/fts/ob_ik_ft_parser.h"
#include "storage/fts/ob_ngram2_ft_parser.h"
#include "storage/fts/ob_ngram_ft_parser.h"
#include "data_plane/fts/ob_fts_parser_property.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_whitespace_ft_parser.h"

namespace oceanbase
{
namespace storage
{

const char *ObFTParser::NAME_STR[ObFTParser::ParserType::FTP_MAX] = {
#define FT_PARSER_TYPE(ftp_type, parser_name) #parser_name,
  FTS_BUILD_IN_PARSER_LIST
#undef FT_PARSER_TYPE
};

bool ObFTParser::is_builtin() const
{
  bool is_builtin = false;
#define FT_PARSER_TYPE(ftp_type, parser_name) is_builtin = is_builtin || is_##parser_name();
  FTS_BUILD_IN_PARSER_LIST
#undef FT_PARSER_TYPE
  return is_builtin;
}

int ObFTParser::init(const common::ObString &parser_name)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(parser_name_.set_name(parser_name))) {
  } else if (!is_builtin()) {
    ret = OB_FUNCTION_NOT_DEFINED;
    LOG_USER_ERROR(OB_FUNCTION_NOT_DEFINED, parser_name.length(), parser_name.ptr());
  } else {
    parser_version_ = BUILTIN_VERSION;
  }
  return ret;
}

int ObFTParser::parse_from_str(const char *parser_name, const int64_t buf_len)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(parser_name)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(buf_len >= OB_FT_PARSER_NAME_LENGTH)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    char name[OB_FT_PARSER_NAME_LENGTH];
    char *saveptr = nullptr;
    char *token = nullptr;
    char *end_ptr = nullptr;
    MEMCPY(name, parser_name, buf_len);
    name[buf_len] = '\0';
    if (OB_ISNULL(token = STRTOK_R(name, ".", &saveptr))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(parser_name_.set_name(token))) {
    } else if (OB_ISNULL(token = STRTOK_R(nullptr, ".", &saveptr))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ob_strtoll(token, end_ptr, parser_version_))) {
    } else if (OB_NOT_NULL(token = STRTOK_R(nullptr, ".", &saveptr))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (!is_builtin()) {
      ret = OB_FUNCTION_NOT_DEFINED;
      LOG_USER_ERROR(OB_FUNCTION_NOT_DEFINED, static_cast<int>(buf_len), parser_name);
    } else if (OB_UNLIKELY(!is_valid())) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

// The fulltext parser name consists of two parts: name and version, e.g. default_parser.1,
// separated by dot. This function is designed to serialize them into cstring.
int ObFTParser::serialize_to_str(char *buf, const int64_t buf_len)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  if (OB_ISNULL(buf) || OB_UNLIKELY(buf_len < OB_FT_PARSER_NAME_LENGTH)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(!is_valid())) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(common::databuff_printf(buf, buf_len, pos, "%.*s.%ld", parser_name_.len(), parser_name_.str(),
          parser_version_))) {
  }
  return ret;
}

int ObFTParser::get_desc(const ObIFTParserDesc *&parser_desc) const
{
  int ret = OB_SUCCESS;
  parser_desc = nullptr;
  if (OB_UNLIKELY(!is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (is_space()) {
    static const ObWhiteSpaceFTParserDesc desc;
    parser_desc = &desc;
  } else if (is_ngram()) {
    static const ObNgramFTParserDesc desc;
    parser_desc = &desc;
  } else if (is_beng()) {
    static const ObBasicEnglishFTParserDesc desc;
    parser_desc = &desc;
  } else if (is_ik()) {
    static const ObIKFTParserDesc desc;
    parser_desc = &desc;
  } else if (is_ngram2()) {
    static const ObNgram2FTParserDesc desc;
    parser_desc = &desc;
  } else {
    ret = OB_FUNCTION_NOT_DEFINED;
  }
  return ret;
}
////////////////////////////////////////////////////////////////////////////////
// ObFTParseData

static ObFTParseData *g_ftparse_data = nullptr;
static constexpr const char *FTPARSE_DATA_MEMORY_LABEL = "FtParse";

int ObFTParseData::init_global()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(g_ftparse_data)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(g_ftparse_data = OB_NEW(ObFTParseData, FTPARSE_DATA_MEMORY_LABEL))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(g_ftparse_data->init())) {
  }
  return ret;
}

void ObFTParseData::deinit_global()
{
  if (OB_NOT_NULL(g_ftparse_data)) {
    OB_DELETE(ObFTParseData, FTPARSE_DATA_MEMORY_LABEL, g_ftparse_data);
    g_ftparse_data = nullptr;
  }
}

ObFTParseData &ObFTParseData::instance()
{
  return *g_ftparse_data;
}

ObFTParseData::~ObFTParseData()
{
  destroy();
}

int ObFTParseData::init()
{
  int ret = OB_SUCCESS;
  lib::ObMemAttr mem_attr;
  mem_attr.label_ = FTPARSE_DATA_MEMORY_LABEL;

  if (OB_FAIL(handler_allocator_.init(lib::ObMallocAllocator::get_instance(),
                                      OB_MALLOC_NORMAL_BLOCK_SIZE,
                                      mem_attr))) {
  } else if (OB_FAIL(init_and_set_stopword_list())) {
  } else if (OB_FAIL(init_dict_hub())) {
  } else {
    is_inited_ = true;
    FLOG_INFO("succeeded to initialize fulltext parser data", KP(this));
  }

  if (OB_UNLIKELY(!is_inited_)) {
    destroy();
  }
  return ret;
}

int ObFTParseData::init_and_set_stopword_list()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(stop_word_checker_ = OB_NEWx(ObStopWordChecker, &handler_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(stop_word_checker_->init())) {
  }

  if (OB_FAIL(ret)) {
    OB_DELETEx(ObStopWordChecker, &handler_allocator_, stop_word_checker_);
    stop_word_checker_ = nullptr;
  }

  return ret;
}

int ObFTParseData::init_dict_hub()
{
  // make dict
  int ret = OB_SUCCESS;
  if (OB_ISNULL(dict_hub_ = OB_NEWx(ObFTDictHub, &handler_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(dict_hub_->init())) {
  }
  return ret;
}

void ObFTParseData::destroy()
{
  if (OB_NOT_NULL(stop_word_checker_)) {
    stop_word_checker_->destroy();
    OB_DELETEx(ObStopWordChecker, &handler_allocator_, stop_word_checker_);
    stop_word_checker_ = nullptr;
  }

  if (!OB_ISNULL(dict_hub_)) {
    dict_hub_->destroy();
    dict_hub_->~ObFTDictHub();
    handler_allocator_.free(dict_hub_);
    dict_hub_ = nullptr;
  }

  handler_allocator_.reset();
  is_inited_ = false;
}

int ObFTParseData::get_dict_hub(ObFTDictHub *&hub)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(dict_hub_)) {
    ret = OB_NOT_INIT;
  } else {
    hub = dict_hub_;
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// ObFTParseHelper
// One runtime per helper, never shared between concurrent executions. In
// particular, none of its parser metadata lives in the caller's document arena.
class FTParseRuntime final
{
public:
  FTParseRuntime()
      : allocator_(ObMemAttr("FTParserState")), ik_(nullptr), beng_(nullptr),
        cs_(nullptr), words_(nullptr), bucket_count_(0), nodes_reserved_(false) {}
  ~FTParseRuntime()
  {
    reset_parser();
    OB_DELETE(ObFTWordMap, "FTWordMap", words_);
  }

  int get_words(int64_t bucket_count, ObFTWordMap *&words, bool exact_bucket_count)
  {
    int ret = OB_SUCCESS;
    const int64_t actual_count = common::hash::cal_next_prime(bucket_count);
    if (nullptr != words_ && ((exact_bucket_count && actual_count != bucket_count_)
        || actual_count > bucket_count_ || words_->size() > 4096 || bucket_count_ > 4096)) {
      OB_DELETE(ObFTWordMap, "FTWordMap", words_);
      words_ = nullptr;
      nodes_reserved_ = false;
    }
    if (nullptr == words_) {
      if (OB_ISNULL(words_ = OB_NEW(ObFTWordMap, "FTWordMap"))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(words_->create(bucket_count, ObMemAttr("FTWordMap")))) {
        OB_DELETE(ObFTWordMap, "FTWordMap", words_);
        words_ = nullptr;
      } else {
        bucket_count_ = actual_count;
      }
    } else if (!nodes_reserved_ && words_->size() > 0) {
      // SimpleAllocer otherwise returns its last block on clear(). Retain just
      // one block, and only once this map has actually served a document.
      using Node = common::hash::HashMapTypes<ObFTWord, int64_t>::AllocType;
      if (OB_SUCC(words_->get_local_allocer().reserve(common::hash::NodeNumTraits<Node>::NODE_NUM))) {
        nodes_reserved_ = true;
      }
    }
    if (OB_SUCC(ret)) {
      words = words_;
    }
    return ret;
  }

  int get_iterator(ObFTParserParam &param, bool is_ik, ObITokenIterator *&iter)
  {
    int ret = OB_SUCCESS;
    if (cs_ != param.cs_) {
      reset_parser();
    }
    if (is_ik) {
      if (nullptr != ik_) {
        ret = ik_->reuse(param);
      } else {
        ObFTDictHub *hub = nullptr;
        if (OB_FAIL(ObFTParseData::instance().get_dict_hub(hub))) {
        } else if (OB_ISNULL(ik_ = OB_NEWx(ObIKFTParser, &allocator_, allocator_, hub, true))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          ret = ik_->init(param);
        }
      }
      iter = ik_;
    } else {
      if (nullptr != beng_) {
        ret = beng_->reuse(&param);
      } else if (OB_ISNULL(beng_ = OB_NEWx(ObBEngFTParser, &allocator_, allocator_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        ret = beng_->init(&param);
      }
      iter = beng_;
    }
    if (OB_FAIL(ret)) {
      reset_parser();
      iter = nullptr;
    } else {
      cs_ = param.cs_;
    }
    return ret;
  }

  void clear_document()
  {
    if (nullptr != ik_) { ik_->clear_document(); }
    if (nullptr != beng_) { beng_->clear_document(); }
  }

private:
  void reset_parser()
  {
    OB_DELETEx(ObIKFTParser, &allocator_, ik_);
    OB_DELETEx(ObBEngFTParser, &allocator_, beng_);
    ik_ = nullptr;
    beng_ = nullptr;
    cs_ = nullptr;
    allocator_.reset();
  }
  common::ObArenaAllocator allocator_;
  ObIKFTParser *ik_;
  ObBEngFTParser *beng_;
  const ObCharsetInfo *cs_;
  ObFTWordMap *words_;
  int64_t bucket_count_;
  bool nodes_reserved_;
};

int ObFTParseHelper::segment(
    const ObFTParserProperty &property,
    const int64_t parser_version,
    const ObIFTParserDesc *parser_desc,
    const ObCharsetInfo *cs,
    const char *ft,
    const int64_t ft_len,
    common::ObIAllocator &allocator,
    ObAddWord &add_word,
    FTParseRuntime *runtime,
    bool is_ik,
    bool is_beng)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(parser_version < 0 || nullptr == parser_desc || nullptr == cs || nullptr == ft || 0 >= ft_len)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObFTParserParam param;
    ObITokenIterator *iter = nullptr;
    param.allocator_ = &allocator;
    param.cs_ = cs;
    param.fulltext_ = ft;
    param.ft_length_ = ft_len;
    param.parser_version_ = parser_version;
    param.ngram_token_size_ = property.ngram_token_size_;
    param.ik_param_.mode_
        = (property.ik_mode_smart_ ? ObFTIKParam::Mode::SMART : ObFTIKParam::Mode::MAX_WORD);
    param.min_ngram_size_ = property.min_ngram_token_size_;
    param.max_ngram_size_ = property.max_ngram_token_size_;
    const bool reuse_parser = nullptr != runtime && (is_ik || is_beng);

    if (OB_FAIL(reuse_parser
                    ? runtime->get_iterator(param, is_ik, iter)
                    : parser_desc->segment(&param, iter))) {
    } else if (OB_ISNULL(iter)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const char *word = nullptr;
      int64_t word_len = 0;
      int64_t char_cnt = 0;
      int64_t word_freq = 0;
      while (OB_SUCC(ret)) {
        if (OB_FAIL(iter->get_next_token(word, word_len, char_cnt, word_freq))) {
          if (OB_ITER_END != ret) {
          }
        } else if (OB_FAIL(add_word.process_word(word, word_len, char_cnt, word_freq))) {
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
    if (OB_NOT_NULL(iter)) {
      if (reuse_parser) {
        runtime->clear_document();
      } else {
        parser_desc->free_token_iter(&param, iter);
      }
      iter = nullptr;
    }
  }
  return ret;
}

ObFTParseHelper::ObFTParseHelper()
    : allocator_(nullptr),
    parser_desc_(nullptr),
    parser_name_(),
    add_word_flag_(),
    parser_property_(),
    is_inited_(false),
    runtime_(nullptr)
{
}

ObFTParseHelper::~ObFTParseHelper()
{
  reset();
}

int ObFTParseHelper::init(
    common::ObIAllocator *allocator,
    const common::ObString &parser_name,
    const common::ObString &parser_properties)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(allocator) || OB_UNLIKELY(parser_name.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parser_name_.parse_from_str(parser_name.ptr(), parser_name.length()))) {
  } else if (OB_FAIL(parser_property_.parse_for_parser_helper(parser_name_, parser_properties))) {
  } else if (OB_FAIL(parser_name_.get_desc(parser_desc_))) {
  } else if (OB_ISNULL(parser_desc_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(set_add_word_flag(*parser_desc_))) {
  } else {
    allocator_ = allocator;
    is_inited_ = true;
  }
  if (OB_FAIL(ret) && OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

void ObFTParseHelper::reset()
{
  OB_DELETE(FTParseRuntime, "FTParserState", runtime_);
  runtime_ = nullptr;
  parser_desc_ = nullptr;
  allocator_ = nullptr;
  add_word_flag_.clear();
  is_inited_ = false;
}

int ObFTParseHelper::init(common::ObIAllocator *allocator, const ObFTParser &parser,
                         const ObFTParserProperty &property)
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else if (nullptr == allocator || !parser.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parser.get_desc(parser_desc_))) {
  } else if (OB_FAIL(set_add_word_flag(*parser_desc_))) {
  } else {
    allocator_ = allocator;
    parser_name_ = parser;
    parser_property_ = property;
    is_inited_ = true;
  }
  return ret;
}

int ObFTParseHelper::segment(const ObObjMeta &meta, const char *fulltext,
                           int64_t fulltext_len, int64_t &doc_length,
                           int64_t bucket_count, ObFTWordMap *&words,
                           bool exact_bucket_count) const
{
  int ret = OB_SUCCESS;
  words = nullptr;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (bucket_count <= 0 || fulltext_len < 0 || (fulltext_len > 0 && nullptr == fulltext)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (nullptr == runtime_
             && OB_ISNULL(runtime_ = OB_NEW(FTParseRuntime, "FTParserState"))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(runtime_->get_words(bucket_count, words, exact_bucket_count))) {
  } else if (0 == fulltext_len) {
    words->reuse();
    doc_length = 0;
  } else {
    ret = segment(meta, fulltext, fulltext_len, doc_length, *words);
  }
  return ret;
}

int ObFTParseHelper::segment(
    const ObObjMeta &meta,
    const char *fulltext,
    const int64_t fulltext_len,
    int64_t &doc_length,
    ObFTWordMap &words) const
{
  int ret = OB_SUCCESS;
  const ObCharsetInfo *cs = nullptr;
  ObCollationType type = meta.get_collation_type();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(allocator_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(CS_TYPE_INVALID == type || type >= CS_TYPE_PINYIN_BEGIN_MARK)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(cs = common::ObCharset::get_charset(type))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    words.reuse();
    ObAddWord add_word(parser_property_, meta, add_word_flag_, *allocator_, words);
    if (OB_FAIL(segment(
                    parser_property_,
                    parser_name_.get_parser_version(),
                    parser_desc_,
                    cs,
                    fulltext,
                    fulltext_len,
                    *allocator_,
                    add_word,
                    runtime_,
                    parser_name_.is_ik(),
                    parser_name_.is_beng()))) {
    } else {
      doc_length = add_word.get_add_word_count();
    }
  }
  LOG_DEBUG("ft parse segment", K(ret), K(type), K(add_word_flag_), K(parser_name_),
      K(ObString(fulltext_len, fulltext)), K(words.size()));
  return ret;
}

int ObFTParseHelper::check_is_the_same(
    const common::ObString &parser_name_str,
    const common::ObString &parser_properties,
    bool &is_same) const
{
  int ret = OB_SUCCESS;
  is_same = false;
  if (is_inited_) {
    storage::ObFTParser parser_name;
    ObFTParserProperty parser_property;
    if (OB_UNLIKELY(parser_name_str.empty())) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(parser_name.parse_from_str(parser_name_str.ptr(), parser_name_str.length()))) {
    } else if (OB_FAIL(parser_property.parse_for_parser_helper(parser_name, parser_properties))) {
    } else if (parser_name == parser_name_ && parser_property.is_equal(parser_property_)) {
      is_same = true;
    }
  }
  return ret;
}

int ObFTParseHelper::make_detail_json(
    const ObFTWordMap &words,
    const int64_t doc_length,
    common::ObIJsonBase *&json_root)
{
 int ret = OB_SUCCESS;

 ObJsonObject *root_obj = nullptr;

 ObJsonInt *cnt = nullptr;

 ObJsonArray *token_array = nullptr;

 if (OB_ISNULL(root_obj = OB_NEWx(ObJsonObject, allocator_, allocator_))) {
   ret = OB_ALLOCATE_MEMORY_FAILED;
 } else if (OB_ISNULL(cnt = OB_NEWx(ObJsonInt, allocator_, doc_length))) {
   ret = OB_ALLOCATE_MEMORY_FAILED;
 } else if (OB_ISNULL(token_array = OB_NEWx(ObJsonArray, allocator_, allocator_))) {
   ret = OB_ALLOCATE_MEMORY_FAILED;
 } else {
   for (ObFTWordMap::const_iterator it = words.begin(); OB_SUCC(ret) && it != words.end(); ++it) {
     ObString key = it->first.get_word().get_string();
     ObJsonObject *node = nullptr;
     ObJsonInt *token_cnt_node = nullptr;
     if (OB_ISNULL(node = OB_NEWx(ObJsonObject, allocator_, allocator_))) {
       ret = OB_ALLOCATE_MEMORY_FAILED;
     } else if (OB_ISNULL(token_cnt_node = OB_NEWx(ObJsonInt, allocator_, it->second))) {
       ret = OB_ALLOCATE_MEMORY_FAILED;
     } else if (OB_FAIL(node->add(key, token_cnt_node))) {
     } else if (OB_FAIL(token_array->append(node))) {
     } else {
       // pass
     }

     if (OB_FAIL(ret)) {
       OB_DELETEx(ObJsonObject, allocator_, node);
       OB_DELETEx(ObJsonInt, allocator_, token_cnt_node);
     }
   } // for

   if (OB_SUCC(ret)) {
     if (OB_FAIL(root_obj->add(ENTRY_NAME_TOKENS, token_array))) {
     } else if (OB_FAIL(root_obj->add(ENTRY_NAME_DOC_LEN, cnt))) {
     }
   }
 }
  if (OB_SUCC(ret)) {
    json_root = root_obj;
  } else {
    OB_DELETEx(ObJsonObject, allocator_, root_obj);
    OB_DELETEx(ObJsonInt, allocator_, cnt);
    OB_DELETEx(ObJsonArray, allocator_, token_array);
  }

  return ret;
}

int ObFTParseHelper::make_token_array_json(
    const ObFTWordMap &words,
    common::ObIJsonBase *&json_root)
{
  int ret = OB_SUCCESS;
  ObJsonArray *token_array = nullptr;
  if (OB_UNLIKELY(OB_ISNULL(token_array = OB_NEWx(ObJsonArray, allocator_, allocator_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    for (ObFTWordMap::const_iterator it = words.begin(); OB_SUCC(ret) && it != words.end(); ++it) {
      ObString key = it->first.get_word().get_string();
      ObJsonString *token = nullptr;
      if (OB_UNLIKELY(OB_ISNULL(token = OB_NEWx(ObJsonString, allocator_, key)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (OB_FAIL(token_array->append(token))) {
          OB_DELETEx(ObJsonString, allocator_, token);
        } else {
        }
      }
    } // for
  }
  if (OB_SUCC(ret)) {
    json_root = token_array;
  } else {
    OB_DELETEx(ObJsonArray, allocator_, token_array);
  }
  return ret;
}

int ObFTParseHelper::set_add_word_flag(const ObIFTParserDesc &ftparser_desc)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ftparser_desc.get_add_word_flag(add_word_flag_))) {
  }
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
