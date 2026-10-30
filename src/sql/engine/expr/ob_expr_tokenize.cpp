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

#include "sql/engine/expr/ob_expr_tokenize.h"
#include "sql/engine/ob_exec_context.h"

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

namespace oceanbase
{
namespace sql
{
// Only immutable built-in defaults are shared. These are the same defaults as
// ObFTParserProperty and ik_rebuild_props_for_ddl; there is no parser, document,
// allocator or execution state in this process-lifetime configuration.
struct TokenizeDefaultConfig final
{
  explicit TokenizeDefaultConfig(const char *name)
  {
    const int ret = parser_.init(ObString::make_string(name));
    OB_ASSERT(OB_SUCCESS == ret);
    const int name_ret = parser_.serialize_to_str(inner_name_, sizeof(inner_name_));
    OB_ASSERT(OB_SUCCESS == name_ret);
    if (parser_.is_ik()) {
      property_.dict_table_ = ObString(storage::ObFTSLiteral::CONFIG_NAME_DICT_TABLE);
      property_.stopword_table_ = ObString(storage::ObFTSLiteral::CONFIG_NAME_STOPWORD_TABLE);
      property_.quantifier_table_ = ObString(storage::ObFTSLiteral::CONFIG_NAME_QUANTIFIER_TABLE);
    }
  }
  storage::ObFTParser parser_;
  storage::ObFTParserProperty property_;
  char inner_name_[storage::OB_FT_PARSER_NAME_LENGTH];
};

class TokenizeExprContext final : public ObExprOperatorCtx
{
public:
  TokenizeExprContext()
      : config_allocator_(ObMemAttr("TokenizeConfig")),
        document_allocator_(ObMemAttr("TokenizeDoc")),
        helper_(), parser_name_(), properties_(), prepared_parser_(nullptr),
        prepared_property_(nullptr), raw_name_(), raw_properties_(), output_mode_(0),
        dictionary_collation_(CS_TYPE_INVALID), ready_(false), key_valid_(false) {}
  void reuse_document()
  {
    if (document_allocator_.total() > (1 << 20)) {
      helper_.clear_document(true);
      document_allocator_.reset();
    } else {
      document_allocator_.reuse();
    }
  }
  ObArenaAllocator config_allocator_;
  ObArenaAllocator document_allocator_;
  storage::ObFTParseHelper helper_;
  ObString parser_name_;
  ObString properties_;
  const storage::ObFTParser *prepared_parser_;
  const storage::ObFTParserProperty *prepared_property_;
  ObString raw_name_;
  ObString raw_properties_;
  int output_mode_;
  ObCollationType dictionary_collation_;
  bool ready_;
  bool key_valid_;
};

// Compare values, never datum addresses: parameters can change on a rescan.
// Out-of-row JSON/LOB locators are deliberately excluded from this fast path.
static int read_tokenize_config_key(const ObExpr &expr, ObEvalCtx &ctx,
                                   const TokenizeExprContext &state, bool cached_name_only,
                                   ObString &name, ObString &properties, bool &comparable)
{
  int ret = OB_SUCCESS;
  comparable = true;
  name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  ObDatum *value = nullptr;
  if (expr.arg_cnt_ >= 2) {
    if (OB_FAIL(expr.args_[1]->eval(ctx, value))) {
    } else if (!value->is_null()) {
      name = value->get_string().trim();
    }
  }
  if (OB_SUCC(ret) && cached_name_only
      && (!state.ready_ || !state.key_valid_ || name != state.raw_name_)) {
    comparable = false;
  }
  if (OB_SUCC(ret) && comparable && expr.arg_cnt_ == 3) {
    const ObObjType type = expr.args_[2]->datum_meta_.type_;
    if (type != ObVarcharType && type != ObCharType) {
      comparable = false;
    } else if (OB_FAIL(expr.args_[2]->eval(ctx, value))) {
    } else if (value->is_null()) {
      comparable = false;
    } else {
      properties = value->get_string();
    }
  }
  return ret;
}

ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  ObIJsonBase *json_result = nullptr;
  TokenizeParam param;
  // Constant arguments describe one input per execution. Keep the inexpensive
  // one-shot allocator path; only the immutable configuration is reused here.
  if (1 == expr.extra_) {
    storage::ObFTParseHelper helper;
    storage::ObFTWordMap words;
    int64_t doc_length = 0;
    if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
    } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
    } else if (OB_FAIL(nullptr != param.prepared_parser_
                          ? helper.init(&temp_allocator, *param.prepared_parser_, *param.prepared_property_)
                          : helper.init(&temp_allocator, param.parser_name_, param.properties_))) {
    } else if (OB_FAIL(words.create(MIN(MAX(param.fulltext_.length() / 2, 2), 997),
                                    ObMemAttr("FTWordMap")))) {
    } else if (!param.fulltext_.empty()
               && OB_FAIL(helper.segment(param.meta_, param.fulltext_.ptr(),
                                          param.fulltext_.length(), doc_length, words))) {
    } else if (OB_FAIL(TokenizeParam::ALL == param.output_mode_
                          ? helper.make_detail_json(words, doc_length, json_result)
                          : helper.make_token_array_json(words, json_result))) {
    } else {
      ret = ObJsonExprHelper::pack_json_res(expr, ctx, temp_allocator, json_result, expr_datum);
    }
    return ret;
  }
  TokenizeExprContext *state = static_cast<TokenizeExprContext *>(
      ctx.exec_ctx_.get_expr_op_ctx(expr.expr_ctx_id_));
  ObString raw_name;
  ObString raw_properties;
  bool comparable = false;

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (nullptr == state && OB_FAIL(ctx.exec_ctx_.create_expr_op_ctx(expr.expr_ctx_id_, state))) {
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(read_tokenize_config_key(expr, ctx, *state, true,
                                             raw_name, raw_properties, comparable))) {
  } else {
    const bool same_config = comparable && raw_properties == state->raw_properties_;
    if (same_config) {
      param.parser_name_ = state->parser_name_;
      param.properties_ = state->properties_;
      param.prepared_parser_ = state->prepared_parser_;
      param.prepared_property_ = state->prepared_property_;
      param.output_mode_ = static_cast<TokenizeParam::OUTPUT_MODE>(state->output_mode_);
    } else {
      ret = parse_param(expr, ctx, temp_allocator, param);
    }
    if (OB_FAIL(ret)) {
      return ret;
    }
    state->reuse_document();
    if (!state->ready_ || state->prepared_parser_ != param.prepared_parser_
        || state->parser_name_ != param.parser_name_ || state->properties_ != param.properties_) {
      state->ready_ = false;
      state->key_valid_ = false;
      state->helper_.reset();
      state->config_allocator_.reset();
      state->dictionary_collation_ = CS_TYPE_INVALID;
      if (OB_FAIL(ob_write_string(state->config_allocator_, param.parser_name_, state->parser_name_))) {
      } else if (OB_FAIL(ob_write_string(state->config_allocator_, param.properties_, state->properties_))) {
      } else if (nullptr != param.prepared_parser_) {
        ret = state->helper_.init(&state->document_allocator_, *param.prepared_parser_,
                                  *param.prepared_property_);
      } else {
        ret = state->helper_.init(&state->document_allocator_, state->parser_name_, state->properties_);
      }
      if (OB_SUCC(ret)) {
        state->prepared_parser_ = param.prepared_parser_;
        state->prepared_property_ = param.prepared_property_;
        state->ready_ = true;
      }
    }
    if (OB_SUCC(ret) && !same_config) {
      state->key_valid_ = false;
      if (OB_FAIL(read_tokenize_config_key(expr, ctx, *state, false,
                                           raw_name, raw_properties, comparable))) {
      } else if (comparable) {
        if (OB_FAIL(ob_write_string(state->config_allocator_, raw_name, state->raw_name_))) {
        } else if (OB_FAIL(ob_write_string(state->config_allocator_, raw_properties, state->raw_properties_))) {
        } else {
          state->output_mode_ = param.output_mode_;
          state->key_valid_ = true;
        }
      }
    }
    if (OB_SUCC(ret) && state->dictionary_collation_ != param.meta_.get_collation_type()) {
      if (OB_FAIL(param.try_load_dictionary_for_ik())) {
      } else {
        state->dictionary_collation_ = param.meta_.get_collation_type();
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, state->helper_, json_result))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_res(expr,
                                                     ctx,
                                                     temp_allocator,
                                                     json_result,
                                                     expr_datum))) {
    }
    // Do not retain exceptionally large dynamic configurations across rows.
    if (state->config_allocator_.total() > (1 << 20)) {
      state->helper_.reset();
      state->config_allocator_.reset();
      state->ready_ = false;
      state->key_valid_ = false;
    }
  }

  return ret;
}

int ObExprTokenize::tokenize_fulltext(const TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      storage::ObFTParseHelper &tokenize_helper,
                                      ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;
  storage::ObFTWordMap *token_map = nullptr;

  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(tokenize_helper.segment(
                     param.meta_,
                     param.fulltext_.ptr(),
                     param.fulltext_.length(),
                     doc_len,
                     ft_word_bkt_cnt,
                     token_map))) {
  } else {
    switch (param.output_mode_) {
    case TokenizeParam::OUTPUT_MODE::DEFAULT: {
      if (OB_FAIL(tokenize_helper.make_token_array_json(*token_map, result))) {
      } else {
        // pass
      }
      break;
    }
    case TokenizeParam::OUTPUT_MODE::ALL: {
      if (OB_FAIL(tokenize_helper.make_detail_json(*token_map, doc_len, result))) {
      } else {
        // pass
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam ::TokenizeParam()
  : allocator_(ObMemAttr("TokenizeParam")),
    parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    prepared_parser_(nullptr),
    prepared_property_(nullptr),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(allocator_, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *parser_params_datum;
  ObString raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_name(expr, ctx, param))) {
  } else if (nullptr == param.prepared_parser_) {
    if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
    } else if (OB_FAIL(param.reform_parser_properties(param.properties_))) {
    }
  }
  return ret;
}

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;

  char *parser_name_buf = nullptr;
  if (OB_ISNULL(parser_name_buf
                = static_cast<char *>(param.allocator_.alloc(storage::OB_FT_PARSER_NAME_LENGTH)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser.init(input_str))) {
  } else if (OB_FAIL(parser.serialize_to_str(parser_name_buf, storage::OB_FT_PARSER_NAME_LENGTH))) {
  } else {
    param.parser_name_ = ObString::make_string(parser_name_buf);
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
    bool constant_input = true;
    for (int64_t i = 0; i < raw_expr.get_param_count(); ++i) {
      constant_input &= raw_expr.get_param_expr(i)->is_const_expr();
    }
    rt_expr.extra_ = constant_input ? 1 : 0;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::parse_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;
  ObString raw_parser_name;

  if (expr.arg_cnt_ < 2) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else {
    if (parser_datum->is_null()) {
      raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    } else {
      ObString name = parser_datum->get_string();
      raw_parser_name = name.trim();
    }
  }

  if (OB_FAIL(ret)) {
    // already logged
  } else if (expr.arg_cnt_ < 3
             && (0 == raw_parser_name.case_compare(storage::ObFTSLiteral::PARSER_NAME_IK)
                 || 0 == raw_parser_name.case_compare(storage::ObFTSLiteral::PARSER_NAME_BENG))) {
    static const TokenizeDefaultConfig ik(storage::ObFTSLiteral::PARSER_NAME_IK);
    static const TokenizeDefaultConfig beng(storage::ObFTSLiteral::PARSER_NAME_BENG);
    const bool is_ik = 0 == raw_parser_name.case_compare(storage::ObFTSLiteral::PARSER_NAME_IK);
    const TokenizeDefaultConfig &config = is_ik ? ik : beng;
    param.prepared_parser_ = &config.parser_;
    param.prepared_property_ = &config.property_;
    // The name is also used by the existing dictionary loader.
    param.parser_name_ = ObString::make_string(config.inner_name_);
  } else if (OB_FAIL(construct_ft_parser_inner_name(raw_parser_name, param))) {
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else {
      if (ObJsonNodeType::J_ARRAY != base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
          ObIJsonBase *node = nullptr;
          if (OB_FAIL(base->get_array_element(i, node))) {
          } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(param.parse_json_param(node))) {
          }
        } // for
      }
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
{
  int ret = OB_SUCCESS;
  storage::ObFTParserJsonProps parser_properties;

  if (OB_FAIL(parser_properties.init())) {
  } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
  } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                             ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                             true))) {
  } else if (OB_FAIL(parser_properties.to_format_json(allocator_, properties_))) {
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  bool need_to_load_dic = false;
  storage::ObDicLoaderHandle dic_loader_handle;
  if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (need_to_load_dic) {
    if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                    ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                    ObCharset::charset_type_by_coll(meta_.get_collation_type()),
                    dic_loader_handle))) {
    } else if (OB_UNLIKELY(!dic_loader_handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dic_loader_handle.get_loader()->try_load_dictionary_in_trans())) {
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
