// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <fmt/core.h>
#include <uv.h>

#include "uvco/loop/scheduler.h"

#include <coroutine>

namespace uvco {

void Scheduler::runAll() {
  while (!resumableActive_.empty()) {
    resumableRunning_.swap(resumableActive_);
    for (auto &coro : resumableRunning_) {
      if (coro == nullptr || coro.done()) {
        continue;
      }
      coro.resume();
    }
    resumableRunning_.clear();
  }
}

void Scheduler::close() { BOOST_ASSERT(resumableActive_.empty()); }

void Scheduler::enqueue(std::coroutine_handle<> handle) {
  // Use of moved-out Scheduler?
  BOOST_ASSERT(resumableActive_.capacity() > 0);
  resumableActive_.push_back(handle);
}

void Scheduler::setUpLoop(uv_loop_t *loop) {}

Scheduler::~Scheduler() = default;

Scheduler::Scheduler() {
  resumableActive_.reserve(16);
  resumableRunning_.reserve(16);
}

void Scheduler::cancel(std::coroutine_handle<> handle) {
  for (auto &resumable :
       std::to_array({&resumableActive_, &resumableRunning_})) {
    for (auto &it : *resumable) {
      if (it == handle) {
        it = nullptr;
      }
    }
  }
}

} // namespace uvco
