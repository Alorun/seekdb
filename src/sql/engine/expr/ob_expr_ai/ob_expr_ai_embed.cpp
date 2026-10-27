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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ob_is_text_tc(types_stack[CONTENT_IDX].get_type())
        ? types_stack[CONTENT_IDX].get_type() : ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(types_stack[DIM_IDX].get_type());
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  res.set_null();
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_content->is_null()) {
    // SQL NULL input propagates without issuing a request.
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or content is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, temp_allocator,
                    *arg_content, expr.args_[CONTENT_IDX]->datum_meta_,
                    expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content))) {
    } else if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      if (OB_FAIL(get_dimension(*arg_dim, expr.args_[DIM_IDX]->datum_meta_.type_, dim))) {
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::get_dimension(const ObDatum &datum, ObObjType type, int64_t &dimension)
{
  int ret = OB_SUCCESS;
  dimension = 0;
  if (datum.is_null() || !ob_is_integer_type(type)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ob_is_unsigned_type(type)) {
    if (datum.get_uint() == 0 || datum.get_uint() > INT64_MAX) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      dimension = datum.get_uint();
    }
  } else if (datum.get_int() <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    dimension = datum.get_int();
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                      const ObBitVector &skip, int64_t batch_size)
{
  int ret = OB_SUCCESS;
  struct EmbedRow {
    int64_t row_;
    int64_t dimension_;
    ObString model_;
    ObString content_;
    ObString result_;
  };
  ObEvalCtx::TempAllocGuard alloc_guard(ctx);
  MultimodeAlloctor allocator(alloc_guard.get_allocator());
  ObEvalCtx::BatchInfoScopeGuard batch_guard(ctx);
  batch_guard.set_batch_size(batch_size);
  ObBitVector &evaluated = expr.get_evaluated_flags(ctx);
  ObDatum *output = expr.locate_batch_datums(ctx);
  ObArray<EmbedRow> rows;
  ObBitVector *pending_skip = nullptr;
  void *bits = nullptr;
  int64_t packet_limit = 0;
  if (batch_size <= 0) {
  } else if (OB_ISNULL(bits = allocator.alloc(ObBitVector::memory_size(batch_size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    pending_skip = to_bit_vector(bits);
    pending_skip->bit_calculate(skip, evaluated, batch_size,
        [](uint64_t left, uint64_t right) { return left | right; });
    const bool has_pending = pending_skip->accumulate_bit_cnt(batch_size) < batch_size;
    for (int64_t arg = 0; OB_SUCC(ret) && has_pending && arg < expr.arg_cnt_; ++arg) {
      if (OB_FAIL(expr.args_[arg]->eval_batch(ctx, *pending_skip, batch_size))) {
      }
    }
    for (int64_t row = 0; OB_SUCC(ret) && row < batch_size; ++row) {
      if (pending_skip->at(row)) {
        continue;
      }
      batch_guard.set_batch_idx(row);
      output[row].set_null();
      ObDatum *content = expr.args_[CONTENT_IDX]->locate_expr_datumvector(ctx).at(row);
      ObDatum *model = expr.args_[MODEL_IDX]->locate_expr_datumvector(ctx).at(row);
      if (content->is_null()) {
        continue;
      }
      EmbedRow input;
      input.row_ = row;
      input.dimension_ = 0;
      input.model_ = model->is_null() ? ObString() : model->get_string();
      if (model->is_null() || input.model_.empty()) {
        ret = OB_INVALID_ARGUMENT;
      } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator,
                     *content, expr.args_[CONTENT_IDX]->datum_meta_,
                     expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), input.content_))) {
      } else if (input.content_.empty()) {
        ret = OB_INVALID_ARGUMENT;
      } else if (expr.arg_cnt_ == 3 && OB_FAIL(get_dimension(
                     *expr.args_[DIM_IDX]->locate_expr_datumvector(ctx).at(row),
                     expr.args_[DIM_IDX]->datum_meta_.type_, input.dimension_))) {
      } else if (OB_FAIL(rows.push_back(input))) {
      }
    }
    if (OB_SUCC(ret) && !rows.empty()) {
      if (OB_ISNULL(ctx.exec_ctx_.get_my_session())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ctx.exec_ctx_.get_my_session()->get_max_allowed_packet(packet_limit))) {
      }
    }
    for (int64_t first = 0; OB_SUCC(ret) && first < rows.count();) {
      // Only adjacent active rows with identical request-level settings share a request.
      // The engine batch bounds the row count; the session packet limit bounds input bytes.
      ObArenaAllocator request_arena(ObMemAttr(N_AI_EMBED));
      MultimodeAlloctor request_allocator(request_arena);
      ObArray<ObString> inputs;
      ObArray<ObString> results;
      ObAIFuncExprInfo *info = nullptr;
      share::ObAiModelEndpointInfo endpoint;
      auto resolver = ::oceanbase::share::server_service<query::ObIAiEndpointResolver>();
      const EmbedRow &settings = rows.at(first);
      int64_t last = first;
      int64_t bytes = 0;
      while (OB_SUCC(ret) && last < rows.count()
             && rows.at(last).model_ == settings.model_
             && rows.at(last).dimension_ == settings.dimension_) {
        const int64_t next_bytes = 6L * rows.at(last).content_.length() + 3;
        if (last > first && bytes + next_bytes > packet_limit) {
          break;
        }
        if (OB_FAIL(inputs.push_back(rows.at(last).content_))) {
        } else {
          bytes += next_bytes;
          ++last;
        }
      }
      ObJsonObject *config = nullptr;
      ObJsonInt *dimension = nullptr;
      if (OB_SUCC(ret) && settings.dimension_ > 0) {
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(request_allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(request_allocator, settings.dimension_, dimension))) {
        } else if (OB_FAIL(config->add("dimensions", dimension))) {
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_ISNULL(resolver)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(request_allocator, settings.model_, info))) {
      } else if (OB_FAIL(resolver->resolve_by_model_name(settings.model_, request_allocator, endpoint))) {
      } else {
        ObAIFuncModel model(request_allocator, *info, endpoint);
        if (OB_FAIL(model.call_dense_embedding_vector_v2(inputs, config, results))) {
        } else if (results.count() != last - first) {
          ret = OB_INVALID_DATA;
        }
        for (int64_t i = first; OB_SUCC(ret) && i < last; ++i) {
          if (OB_FAIL(ob_write_string(allocator, results.at(i - first), rows.at(i).result_))) {
          }
        }
      }
      first = last;
    }
    // Publish only after every request in this invocation has succeeded.
    for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
      const int64_t row = rows.at(i).row_;
      batch_guard.set_batch_idx(row);
      if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, output[row], rows.at(i).result_))) {
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t row = 0; row < batch_size; ++row) {
        if (!pending_skip->at(row)) {
          evaluated.set(row);
        }
      }
    } else {
      for (int64_t row = 0; row < batch_size; ++row) {
        if (!pending_skip->at(row)) {
          output[row].set_null();
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
