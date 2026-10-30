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

#ifndef _OCEANBASE_STORAGE_FTS_IK_OB_IK_CJK_PROCESSOR_H_
#define _OCEANBASE_STORAGE_FTS_IK_OB_IK_CJK_PROCESSOR_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/ik/ob_ik_candidate_automaton.h"
#include "storage/fts/ik/ob_ik_processor.h"

namespace oceanbase
{
namespace storage
{
class ObFTDictHub;
class ObIKCJKProcessor : public ObIIKProcessor
{
public:
  static const int64_t TRANSITION_CACHE_SIZE = 64;
  struct TransitionCacheEntry
  {
    uint32_t state_;
    uint32_t symbol_;
    uint32_t target_;
    bool valid_;
  };

  ObIKCJKProcessor(const ObIFTDict &dict_main,
                   ObIAllocator &alloc,
                   const IKCandidateAutomaton *automaton = nullptr)
      : hits_(alloc), dict_main_(dict_main), automaton_(automaton), automaton_state_(0),
        cjk_start_(-1), cjk_end_(-1)
  {
    MEMSET(transition_cache_, 0, sizeof(transition_cache_));
  }
  ~ObIKCJKProcessor() override { hits_.reset(); }
  void clear_document() override
  {
    hits_.reset();
    automaton_state_ = 0;
    cjk_start_ = cjk_end_ = -1;
  }

public:
  int do_process(TokenizeContext &ctx,
                 const char *ch,
                 const uint8_t char_len,
                 const ObFTCharUtil::CharType type) override;

private:
  ObList<ObDATrieHit, ObIAllocator> hits_;
  const ObIFTDict &dict_main_;
  const IKCandidateAutomaton *automaton_;
  uint32_t automaton_state_;
  TransitionCacheEntry transition_cache_[TRANSITION_CACHE_SIZE];
  int64_t cjk_start_;
  int64_t cjk_end_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObIKCJKProcessor);
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_IK_OB_IK_CJK_PROCESSOR_H_
