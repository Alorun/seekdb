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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
    minimum_should_match_ = param.minimum_should_match_;
    // A small, unscored natural-language query only needs a sorted set union.
    // Keep Boolean evaluation, scoring, and function lookup on the general path.
    unscored_union_ = mode_flag_ == sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE
        && !function_lookup_mode_ && dim_iters_->count() > 0 && dim_iters_->count() <= 8
        && nullptr == param.base_param_->relevance_expr_
        && !param.base_param_->need_project_relevance() && !param.base_param_->need_filter()
        && !param.base_param_->need_pushdown_topk();
    if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                       *param.allocator_, *param.relevance_collector_,
                                       !unscored_union_))) {
    } else if (nullptr != param.base_param_->relevance_expr_
               && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
    } else if (unscored_union_) {
      const sql::ObDatumMeta &meta = param.base_param_->id_proj_expr_->datum_meta_;
      common::ObDatumBasicFuncs *basic_funcs = ObDatumFuncs::get_basic_func(meta.type_, meta.cs_type_);
      if (OB_ISNULL(basic_funcs) || OB_ISNULL(basic_funcs->null_first_cmp_)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        id_cmp_func_ = basic_funcs->null_first_cmp_;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < iter_domain_ids_.count(); ++i) {
        iter_domain_ids_[i] = nullptr;
      }
    }
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  bm25_param_estimator_.reset();
  unscored_union_ = false;
  minimum_should_match_ = 0;
  id_cmp_func_ = nullptr;
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
  if (unscored_union_) {
    for (int64_t i = 0; i < iter_domain_ids_.count(); ++i) {
      iter_domain_ids_[i] = nullptr;
    }
  }
}

int ObTextDaaTIter::do_one_merge_round(int64_t &count)
{
  int ret = OB_SUCCESS;
  if (!unscored_union_) {
    ret = ObSRDaaTIterImpl::do_one_merge_round(count);
  } else {
    // Advance only the lists that contributed to the previous document.
    // Their current IDs remain owned by the token iterators until this point.
    for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
      const int64_t idx = next_round_iter_idxes_[i];
      ObISRDaaTDimIter *iter = (idx >= 0 && idx < dim_iters_->count()) ? dim_iters_->at(idx) : nullptr;
      if (OB_ISNULL(iter)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(iter->get_next_row())) {
        if (OB_ITER_END == ret) {
          iter_domain_ids_[idx] = nullptr;
          ret = OB_SUCCESS;
        }
      } else if (OB_FAIL(iter->get_curr_id(iter_domain_ids_[idx]))) {
      } else if (OB_ISNULL(iter_domain_ids_[idx])) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    next_round_cnt_ = 0;
    const ObDatum *minimum = nullptr;
    for (int64_t i = 0; OB_SUCC(ret) && i < iter_domain_ids_.count(); ++i) {
      const ObDatum *id = iter_domain_ids_[i];
      int cmp = 0;
      if (nullptr == id) {
      } else if (nullptr != minimum && OB_FAIL(id_cmp_func_(*id, *minimum, cmp, nullptr))) {
      } else if (nullptr == minimum || cmp < 0) {
        minimum = id;
        next_round_cnt_ = 1;
        next_round_iter_idxes_[0] = i;
      } else if (0 == cmp) {
        next_round_iter_idxes_[next_round_cnt_++] = i;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (nullptr == minimum) {
      ret = OB_ITER_END;
    } else if (next_round_cnt_ >= minimum_should_match_) {
      ret = cache_result(count, *minimum, 0.0);
    }
  }
  return ret;
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->relevance_expr_ != nullptr) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
