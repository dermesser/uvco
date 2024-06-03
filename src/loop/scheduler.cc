// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <fmt/core.h>
#include <functional>
#include <uv.h>

#include "loop/scheduler.h"

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <span>

namespace uvco {

namespace {

using BloomFilter = std::size_t;

bool haveSeenOrAdd(BloomFilter &filter, std::coroutine_handle<> handle) {
  static_assert(sizeof(BloomFilter) == 8, "BloomFilter is not 64 bits");

  const size_t hash = std::hash<std::coroutine_handle<>>{}(handle);
  const unsigned index1 = hash % 64;
  const unsigned index2 = (hash >> 6U) % 64;
  const unsigned index3 = (hash >> 12U) % 64;
  const unsigned index4 = (hash >> 18U) % 64;
  const unsigned index5 = (hash >> 24U) % 64;

  // More than the first 32 bits appear to not gain much.
  const size_t bloomIndex = (1U << index1) | (1U << index2) | (1U << index3) |
                            (1U << index4) | (1U << index5);

  if ((filter & bloomIndex) == bloomIndex) {
    // Potentially a false positive.
    return true;
  }
  // Definitely not seen before.
  filter |= bloomIndex;
  return false;
}

unsigned findFirstIndexOf(std::span<const std::coroutine_handle<>> handles,
                          std::coroutine_handle<> handle) {
  return std::ranges::find_if(
             handles, [&handle](const auto &h) { return h == handle; }) -
         handles.begin();
}

} // namespace

void Scheduler::runAll() {
  while (!resumableActive_.empty()) {
    BloomFilter seenHandles = 0;
    resumableRunning_.swap(resumableActive_);
    for (unsigned i = 0; i < resumableRunning_.size(); ++i) {
      auto &coro = resumableRunning_[i];

      // Defend against resuming the same coroutine twice in the same loop pass.
      // This happens when SelectSet selects two coroutines which return at the
      // same time. Resuming the same handle twice is not good, very bad, and
      // will usually at least cause a heap use-after-free.

      // Explicitly written in an explicit way :)
      if (!haveSeenOrAdd(seenHandles, coro)) [[likely]] {
        coro.resume();
      } else if (findFirstIndexOf(resumableRunning_, coro) == i) {
        // This is only true if the coroutine is a false positive in the bloom
        // filter, and has not been run before. The linear search is slow (but
        // not too slow), and only happens in the case of a false positive.
        coro.resume();
      } else {
        // This is most likely a SelectSet being awaited, with two coroutines
        // being ready at the same time.
      }
    }
    resumableRunning_.clear();
  }
}

void Scheduler::close() { BOOST_ASSERT(resumableActive_.empty()); }

void Scheduler::enqueue(std::coroutine_handle<> handle) {
  // Use of moved-out Scheduler?
  BOOST_ASSERT(resumableActive_.capacity() != 0);

  if (run_mode_ == RunMode::Immediate) {
    handle.resume();
    return;
  }

  resumableActive_.push_back(handle);
}

void Scheduler::setUpLoop(uv_loop_t *loop) { uv_loop_set_data(loop, this); }

Scheduler::~Scheduler() = default;

Scheduler::Scheduler(RunMode mode) : run_mode_{mode} {
  static constexpr size_t resumableBufferSize = 16;
  resumableActive_.reserve(resumableBufferSize);
  resumableRunning_.reserve(resumableBufferSize);
}

} // namespace uvco
