// Copyright (c) 2026 OceanBase.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <stdint.h>
#include <limits>
#include "lib/ob_errno.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"

namespace oceanbase { namespace share {

enum class AiColumnState : int64_t { PENDING = 0, READY = 1, FAILED = 2 };
enum class AiTaskState : int64_t {
  QUEUED = 1, RUNNING = 2, RETRY_WAIT = 3, STAGED_SUCCESS = 4,
  STAGED_FAILED = 5, SUPERSEDED = 6, RAW_READY = 7
};
inline bool ai_task_terminal(const AiTaskState state)
{
  return state == AiTaskState::STAGED_SUCCESS || state == AiTaskState::STAGED_FAILED
      || state == AiTaskState::SUPERSEDED;
}

// Stable, bounded on-row fence. All integers are big endian. No native object
// layout, pointers, LOB locators or execution-local type IDs enter this format.
struct AiColumnSpec
{
  static constexpr int64_t UUID_SIZE = 16;
  static constexpr int64_t ENCODED_SIZE = 4 + UUID_SIZE + 8 + 8;
  char incarnation_[UUID_SIZE] = {};
  uint64_t generation_ = 0;
  int64_t definition_version_ = 0;

  int advance()
  {
    int ret = common::OB_SUCCESS;
    if (generation_ == std::numeric_limits<uint64_t>::max()) {
      ret = common::OB_SIZE_OVERFLOW;
    } else {
      ++generation_;
    }
    return ret;
  }
  bool same_incarnation(const AiColumnSpec &other) const
  { return MEMCMP(incarnation_, other.incarnation_, UUID_SIZE) == 0; }
  bool operator==(const AiColumnSpec &other) const
  {
    return same_incarnation(other) && generation_ == other.generation_
        && definition_version_ == other.definition_version_;
  }
  int encode(common::ObIAllocator &allocator, common::ObString &out) const
  {
    int ret = common::OB_SUCCESS;
    char *buf = nullptr;
    out.reset();
    if (generation_ == 0 || definition_version_ <= 0) {
      ret = common::OB_INVALID_ARGUMENT;
    } else if (nullptr == (buf = static_cast<char *>(allocator.alloc(ENCODED_SIZE)))) {
      ret = common::OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(buf, "AIC1", 4);
      MEMCPY(buf + 4, incarnation_, UUID_SIZE);
      put_u64(buf + 4 + UUID_SIZE, generation_);
      put_u64(buf + 4 + UUID_SIZE + 8, static_cast<uint64_t>(definition_version_));
      out.assign_ptr(buf, ENCODED_SIZE);
    }
    return ret;
  }
  int decode(const common::ObString &in)
  {
    int ret = common::OB_SUCCESS;
    AiColumnSpec decoded;
    if (in.length() != ENCODED_SIZE || in.ptr() == nullptr || MEMCMP(in.ptr(), "AIC1", 4) != 0) {
      ret = common::OB_INVALID_DATA;
    } else {
      MEMCPY(decoded.incarnation_, in.ptr() + 4, UUID_SIZE);
      decoded.generation_ = get_u64(in.ptr() + 4 + UUID_SIZE);
      const uint64_t def = get_u64(in.ptr() + 4 + UUID_SIZE + 8);
      if (decoded.generation_ == 0 || def == 0 || def > INT64_MAX) {
        ret = common::OB_INVALID_DATA;
      } else {
        decoded.definition_version_ = static_cast<int64_t>(def);
        *this = decoded;
      }
    }
    return ret;
  }
private:
  static void put_u64(char *p, uint64_t value)
  {
    for (int i = 7; i >= 0; --i) { p[i] = static_cast<char>(value & 255); value >>= 8; }
  }
  static uint64_t get_u64(const char *p)
  {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) { value = (value << 8) | static_cast<unsigned char>(p[i]); }
    return value;
  }
};

} } // namespace oceanbase::share
