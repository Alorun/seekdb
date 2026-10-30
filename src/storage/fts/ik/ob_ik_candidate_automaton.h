#pragma once

#include "lib/allocator/page_arena.h"
#include "storage/fts/dict/ob_ik_dic.h"

#include <atomic>
#include <cstdint>

namespace oceanbase
{
namespace storage
{

class ObIKCJKProcessor;

class IKCandidateAutomaton final
{
public:
  static IKCandidateAutomaton &get_instance();

  int build_if_needed(const ObIKDictLoader::RawDict &dict, const int64_t memory_budget);
  bool is_ready() const { return READY == status_.load(std::memory_order_acquire); }
  int advance(uint32_t &state, const char *ch, const uint8_t char_len) const;
  uint32_t first_output_state(const uint32_t state) const;
  uint32_t next_output_state(const uint32_t state) const;
  uint32_t byte_length(const uint32_t state) const;
  uint32_t char_count(const uint32_t state) const;

private:
  friend class ObIKCJKProcessor;

  enum BuildStatus : int32_t
  {
    UNBUILT = 0,
    BUILDING = 1,
    READY = 2,
    DISABLED = 3,
  };

  struct State
  {
    uint32_t edge_begin_;
    uint32_t edge_count_;
    uint32_t fail_state_;
    uint32_t output_state_;
    uint32_t byte_length_;
    uint32_t char_count_;
    bool is_terminal_;
  };

  struct Edge
  {
    uint32_t parent_;
    uint32_t symbol_;
    uint32_t target_;
  };

  IKCandidateAutomaton();
  int build_(const ObIKDictLoader::RawDict &dict, const int64_t memory_budget);
  int advance_ready_(uint32_t &state, const char *ch, const uint8_t char_len) const;
  int advance_ready_(uint32_t &state, const uint32_t symbol) const;
  uint32_t first_output_state_ready_(const uint32_t state) const;
  uint32_t next_output_state_ready_(const uint32_t state) const;
  uint32_t byte_length_ready_(const uint32_t state) const;
  uint32_t char_count_ready_(const uint32_t state) const;
  static uint32_t pack_symbol_(const char *ch, const uint8_t char_len);
  // Callers validate states and provide initialized automaton storage.
  static bool find_transition_(const State *states,
                              const Edge *edges,
                              const uint32_t state,
                              const uint32_t symbol,
                              uint32_t &target);

private:
  std::atomic<int32_t> status_;
  common::ObArenaAllocator allocator_;
  State *states_;
  Edge *edges_;
  // Only the root has a large fanout. Keep other states sparse.
  static constexpr uint32_t ROOT_TRANSITION_COUNT = 0x10000;
  uint32_t *root_transitions_;
  uint32_t state_count_;
  uint32_t edge_count_;
};

} // namespace storage
} // namespace oceanbase
