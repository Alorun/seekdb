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

#include "storage/fts/ik/ob_ik_candidate_automaton.h"

#include "lib/charset/ob_charset.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"

#include <algorithm>
#include <thread>

using namespace oceanbase::common;

namespace oceanbase
{
namespace storage
{

namespace
{
int get_word_char_count(const ObString &word, int64_t &char_count)
{
  int ret = OB_SUCCESS;
  char_count = 0;
  for (int64_t offset = 0; OB_SUCC(ret) && offset < word.length();) {
    int64_t char_len = 0;
    if (OB_FAIL(ObCharset::first_valid_char(ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                            word.ptr() + offset,
                                            word.length() - offset,
                                            char_len))) {
    } else if (char_len <= 0) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      offset += char_len;
      ++char_count;
    }
  }
  return ret;
}
}

IKCandidateAutomaton &IKCandidateAutomaton::get_instance()
{
  static IKCandidateAutomaton automaton;
  return automaton;
}

IKCandidateAutomaton::IKCandidateAutomaton()
  : status_(UNBUILT),
    allocator_(lib::ObMemAttr("IKCandidateAC")),
    states_(nullptr),
    edges_(nullptr),
    state_count_(0),
    edge_count_(0)
{}

int IKCandidateAutomaton::build_if_needed(
    const ObIKDictLoader::RawDict &dict,
    const int64_t memory_budget)
{
  int ret = OB_SUCCESS;
  int32_t expected = UNBUILT;
  if (status_.compare_exchange_strong(expected, BUILDING, std::memory_order_acq_rel)) {
    ret = build_(dict, memory_budget);
    if (OB_FAIL(ret)) {
      allocator_.reset();
      states_ = nullptr;
      edges_ = nullptr;
      state_count_ = 0;
      edge_count_ = 0;
      status_.store(DISABLED, std::memory_order_release);
    } else {
      status_.store(READY, std::memory_order_release);
    }
  } else {
    while (BUILDING == status_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    if (READY != status_.load(std::memory_order_acquire)) {
      ret = OB_NOT_SUPPORTED;
    }
  }
  return ret;
}

int IKCandidateAutomaton::build_(
    const ObIKDictLoader::RawDict &dict,
    const int64_t memory_budget)
{
  int ret = OB_SUCCESS;
  int64_t total_char_count = 0;
  int64_t max_word_char_count = 0;
  if (OB_ISNULL(dict.data_) || dict.array_size_ <= 0 || memory_budget <= 0) {
    ret = OB_INVALID_ARGUMENT;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dict.array_size_; ++i) {
    ObString word(dict.data_[i]);
    int64_t word_char_count = 0;
    if (OB_ISNULL(dict.data_[i]) || word.empty()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(get_word_char_count(word, word_char_count))) {
    } else if (word_char_count <= 0 || total_char_count > UINT32_MAX - word_char_count) {
      ret = OB_NOT_SUPPORTED;
    } else {
      total_char_count += word_char_count;
      max_word_char_count = MAX(max_word_char_count, word_char_count);
    }
  }

  const int64_t max_state_count = total_char_count + 1;
  const int64_t max_edge_count = total_char_count;
  const int64_t workspace_size = max_state_count * sizeof(State)
                               + max_edge_count * sizeof(Edge)
                               + max_state_count * sizeof(uint32_t)
                               + (max_word_char_count + 1) * sizeof(uint32_t);
  const int64_t output_size = max_state_count * sizeof(State)
                           + max_edge_count * sizeof(Edge);
  if (OB_SUCC(ret) && (max_state_count > UINT32_MAX
                       || workspace_size > memory_budget
                       || output_size > memory_budget - workspace_size)) {
    ret = OB_NOT_SUPPORTED;
  }

  ObArenaAllocator build_allocator(lib::ObMemAttr("IKACBuild"));
  State *states = nullptr;
  Edge *edges = nullptr;
  uint32_t *queue = nullptr;
  uint32_t *path = nullptr;
  if (OB_SUCC(ret)) {
    if (OB_ISNULL(states = static_cast<State *>(build_allocator.alloc(max_state_count * sizeof(State))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_ISNULL(edges = static_cast<Edge *>(build_allocator.alloc(max_edge_count * sizeof(Edge))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_ISNULL(queue = static_cast<uint32_t *>(build_allocator.alloc(max_state_count * sizeof(uint32_t))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_ISNULL(path = static_cast<uint32_t *>(build_allocator.alloc((max_word_char_count + 1) * sizeof(uint32_t))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMSET(states, 0, max_state_count * sizeof(State));
      path[0] = 0;
    }
  }

  uint32_t next_state = 1;
  uint32_t next_edge = 0;
  int64_t path_count = 1;
  ObString previous_word;
  for (int64_t i = 0; OB_SUCC(ret) && i < dict.array_size_; ++i) {
    const ObString word(dict.data_[i]);
    int64_t common_char_count = 0;
    int64_t previous_offset = 0;
    int64_t word_offset = 0;
    if (!previous_word.empty() && previous_word.compare(word) > 0) {
      ret = OB_NOT_SUPPORTED;
    }
    while (OB_SUCC(ret) && previous_offset < previous_word.length() && word_offset < word.length()) {
      int64_t previous_char_len = 0;
      int64_t word_char_len = 0;
      if (OB_FAIL(ObCharset::first_valid_char(ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                              previous_word.ptr() + previous_offset,
                                              previous_word.length() - previous_offset,
                                              previous_char_len))) {
      } else if (OB_FAIL(ObCharset::first_valid_char(ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                     word.ptr() + word_offset,
                                                     word.length() - word_offset,
                                                     word_char_len))) {
      } else if (previous_char_len != word_char_len
                 || 0 != MEMCMP(previous_word.ptr() + previous_offset,
                                word.ptr() + word_offset,
                                word_char_len)) {
        break;
      } else {
        previous_offset += previous_char_len;
        word_offset += word_char_len;
        ++common_char_count;
      }
    }
    if (OB_SUCC(ret) && common_char_count + 1 > path_count) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_SUCC(ret)) {
      path_count = common_char_count + 1;
      for (int64_t offset = word_offset; OB_SUCC(ret) && offset < word.length();) {
        int64_t char_len = 0;
        if (OB_FAIL(ObCharset::first_valid_char(ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                word.ptr() + offset,
                                                word.length() - offset,
                                                char_len))) {
        } else if (next_state >= max_state_count || next_edge >= max_edge_count) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          const uint32_t parent = path[path_count - 1];
          const uint32_t child = next_state++;
          states[child] = State{};
          edges[next_edge++] = Edge{parent, pack_symbol_(word.ptr() + offset, char_len), child};
          path[path_count++] = child;
          offset += char_len;
        }
      }
      if (OB_SUCC(ret)) {
        const uint32_t terminal_state = path[path_count - 1];
        states[terminal_state].byte_length_ = word.length();
        states[terminal_state].char_count_ = path_count - 1;
        states[terminal_state].is_terminal_ = true;
        previous_word = word;
      }
    }
  }

  if (OB_SUCC(ret)) {
    std::sort(edges, edges + next_edge, [](const Edge &lhs, const Edge &rhs) {
      return lhs.parent_ != rhs.parent_ ? lhs.parent_ < rhs.parent_ : lhs.symbol_ < rhs.symbol_;
    });
    for (uint32_t i = 0; OB_SUCC(ret) && i < next_edge; ++i) {
      const uint32_t parent = edges[i].parent_;
      if (0 == states[parent].edge_count_) {
        states[parent].edge_begin_ = i;
      }
      ++states[parent].edge_count_;
    }
  }

  uint32_t queue_head = 0;
  uint32_t queue_tail = 0;
  if (OB_SUCC(ret)) {
    const State &root = states[0];
    for (uint32_t i = 0; i < root.edge_count_; ++i) {
      const uint32_t child = edges[root.edge_begin_ + i].target_;
      states[child].fail_state_ = 0;
      states[child].output_state_ = 0;
      queue[queue_tail++] = child;
    }
  }
  while (OB_SUCC(ret) && queue_head < queue_tail) {
    const uint32_t parent = queue[queue_head++];
    const State &parent_state = states[parent];
    for (uint32_t i = 0; OB_SUCC(ret) && i < parent_state.edge_count_; ++i) {
      const Edge &edge = edges[parent_state.edge_begin_ + i];
      uint32_t fail_state = parent_state.fail_state_;
      uint32_t fail_target = 0;
      bool found = find_transition_(states, edges, fail_state, edge.symbol_, fail_target);
      while (!found && 0 != fail_state) {
        fail_state = states[fail_state].fail_state_;
        found = find_transition_(states, edges, fail_state, edge.symbol_, fail_target);
      }
      states[edge.target_].fail_state_ = found ? fail_target : 0;
      const uint32_t output_state = states[edge.target_].fail_state_;
      states[edge.target_].output_state_ = states[output_state].is_terminal_
                                             ? output_state : states[output_state].output_state_;
      if (queue_tail >= next_state) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        queue[queue_tail++] = edge.target_;
      }
    }
  }

  if (OB_SUCC(ret)) {
    State *final_states = static_cast<State *>(allocator_.alloc(next_state * sizeof(State)));
    Edge *final_edges = static_cast<Edge *>(allocator_.alloc(next_edge * sizeof(Edge)));
    if (OB_ISNULL(final_states) || (next_edge > 0 && OB_ISNULL(final_edges))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(final_states, states, next_state * sizeof(State));
      if (next_edge > 0) {
        MEMCPY(final_edges, edges, next_edge * sizeof(Edge));
      }
      states_ = final_states;
      edges_ = final_edges;
      state_count_ = next_state;
      edge_count_ = next_edge;
    }
  }
  LOG_INFO("build IK candidate automaton", K(ret), K(dict.array_size_), K(total_char_count),
           K(next_state), K(next_edge), K(workspace_size), K(output_size));
  return ret;
}

uint32_t IKCandidateAutomaton::pack_symbol_(const char *ch, const uint8_t char_len)
{
  const uint32_t b0 = static_cast<unsigned char>(ch[0]);
  uint32_t symbol = b0;
  if (2 == char_len) {
    symbol = ((b0 & 0x1f) << 6)
           | (static_cast<uint32_t>(static_cast<unsigned char>(ch[1])) & 0x3f);
  } else if (3 == char_len) {
    symbol = ((b0 & 0x0f) << 12)
           | ((static_cast<uint32_t>(static_cast<unsigned char>(ch[1])) & 0x3f) << 6)
           | (static_cast<uint32_t>(static_cast<unsigned char>(ch[2])) & 0x3f);
  } else if (4 == char_len) {
    symbol = ((b0 & 0x07) << 18)
           | ((static_cast<uint32_t>(static_cast<unsigned char>(ch[1])) & 0x3f) << 12)
           | ((static_cast<uint32_t>(static_cast<unsigned char>(ch[2])) & 0x3f) << 6)
           | (static_cast<uint32_t>(static_cast<unsigned char>(ch[3])) & 0x3f);
  }
  return symbol;
}

bool IKCandidateAutomaton::find_transition_(const State *states,
                                           const Edge *edges,
                                           const uint32_t state,
                                           const uint32_t symbol,
                                           uint32_t &target)
{
  bool found = false;
  if (nullptr != states && nullptr != edges && state < UINT32_MAX) {
    uint32_t begin = states[state].edge_begin_;
    uint32_t end = begin + states[state].edge_count_;
    while (begin < end) {
      const uint32_t mid = begin + (end - begin) / 2;
      if (edges[mid].symbol_ < symbol) {
        begin = mid + 1;
      } else {
        end = mid;
      }
    }
    if (begin < states[state].edge_begin_ + states[state].edge_count_
        && edges[begin].symbol_ == symbol) {
      target = edges[begin].target_;
      found = true;
    }
  }
  return found;
}

int IKCandidateAutomaton::advance(uint32_t &state, const char *ch, const uint8_t char_len) const
{
  int ret = OB_SUCCESS;
  if (!is_ready() || OB_ISNULL(ch) || char_len <= 0 || char_len > 4
      || state >= state_count_) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const uint32_t symbol = pack_symbol_(ch, char_len);
    uint32_t target = 0;
    bool found = find_transition_(states_, edges_, state, symbol, target);
    while (!found && 0 != state) {
      state = states_[state].fail_state_;
      found = find_transition_(states_, edges_, state, symbol, target);
    }
    state = found ? target : 0;
  }
  return ret;
}

uint32_t IKCandidateAutomaton::first_output_state(const uint32_t state) const
{
  uint32_t result = 0;
  if (is_ready() && state < state_count_) {
    result = states_[state].is_terminal_ ? state : states_[state].output_state_;
  }
  return result;
}

uint32_t IKCandidateAutomaton::next_output_state(const uint32_t state) const
{
  return is_ready() && state < state_count_ ? states_[state].output_state_ : 0;
}

uint32_t IKCandidateAutomaton::byte_length(const uint32_t state) const
{
  return is_ready() && state < state_count_ ? states_[state].byte_length_ : 0;
}

uint32_t IKCandidateAutomaton::char_count(const uint32_t state) const
{
  return is_ready() && state < state_count_ ? states_[state].char_count_ : 0;
}

} // namespace storage
} // namespace oceanbase
