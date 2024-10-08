// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <fmt/core.h>
#include <uv.h>

#include "uvco/loop/scheduler.h"

#include <algorithm>
#include <coroutine>
#include <span>

namespace uvco {

namespace {

unsigned findFirstIndexOf(std::span<const std::coroutine_handle<>> handles,
                          std::coroutine_handle<> handle) {
  return std::ranges::find_if(
             handles, [&handle](const auto &h) { return h == handle; }) -
         handles.begin();
}

} // namespace

void Scheduler::runAll() {
  // In order to not delay checking for new I/O in the UV loop, we only run up
  // to a fixed number of times.
  static constexpr unsigned maxTurnsBeforeReturning = 5;
  unsigned turns = 0;

  while (!resumableActive_.empty() && turns < maxTurnsBeforeReturning) {
    resumableRunning_.swap(resumableActive_);
    for (unsigned i = 0; i < resumableRunning_.size(); ++i) {
      auto &coro = resumableRunning_[i];
      // Defend against resuming the same coroutine twice in the same loop pass.
      // This happens when SelectSet selects two coroutines which return at the
      // same time. Resuming the same handle twice is not good, very bad, and
      // will usually at least cause a heap use-after-free.

      // Check if this coroutine handle has already been resumed. This has
      // quadratic complexity, but appears to be faster than e.g. a Bloom
      // filter, because it takes fewer calculations and is a nice linear search
      // over a usually short vector.
      if (findFirstIndexOf(resumableRunning_, coro) == i) {
        // This is only true if the coroutine is a false positive in the bloom
        // filter, and has not been run before. The linear search is slow (but
        // not too slow), and only happens in the case of a false positive.
        coro.resume();
      }
    }
    resumableRunning_.clear();
    ++turns;
  }
}

void Scheduler::close() { BOOST_ASSERT(resumableActive_.empty()); }

void Scheduler::enqueue(std::coroutine_handle<> handle) {
  // Use of moved-out Scheduler?
  BOOST_ASSERT(resumableActive_.capacity() > 0);
  resumableActive_.push_back(handle);
}

void Scheduler::setUpLoop(uv_loop_t *loop) { uv_loop_set_data(loop, this); }

Scheduler::~Scheduler() = default;

Scheduler::Scheduler() {
  resumableActive_.reserve(16);
  resumableRunning_.reserve(16);
}

} // namespace uvco
