#pragma once

#include "lib/allocator/page_arena.h"
#include "storage/fts/dict/ob_ik_dic.h"

#include <atomic>
#include <cstdint>

namespace oceanbase
{
namespace storage
{

class IKCandidateAutomaton final
{
public:
  static IKCandidateAutomaton &get_instance();

  int build_if_needed(const ObIKDictLoader::RawDict &dict, const int64_t memory_budget);
  bool is_ready() const { return READY == status_.load(std::memory_order_acquire); }
  int advance(uint32_t &state, const char *ch, const uint8_t char_len) const;
  static uint32_t pack_symbol(const char *ch, const uint8_t char_len);
  uint32_t first_output_state(const uint32_t state) const;
  uint32_t next_output_state(const uint32_t state) const;
  uint32_t byte_length(const uint32_t state) const;
  uint32_t char_count(const uint32_t state) const;

private:
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
  uint32_t state_count_;
  uint32_t edge_count_;
};

} // namespace storage
} // namespace oceanbase
