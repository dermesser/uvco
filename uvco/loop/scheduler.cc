// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <array>
#include <cstdint>
#include <fmt/base.h>
#include <fmt/core.h>
#include <uv.h>

#include "uvco/loop/scheduler.h"

#include <coroutine>

namespace uvco {

static constexpr bool logSchedulerOperations = false;

void Scheduler::runAll() {
  while (!resumableActive_.empty()) {
    resumableRunning_.swap(resumableActive_);
    for (auto &handle : resumableRunning_) {
      if (handle == nullptr || handle.done()) {
        continue;
      }

      if constexpr (logSchedulerOperations) {
        fmt::print("Resuming coroutine {:x}\n", (uintptr_t)handle.address());
      }

      handle.resume();
    }
    resumableRunning_.clear();
  }
}

void Scheduler::close() { BOOST_ASSERT(resumableActive_.empty()); }

void Scheduler::enqueue(std::coroutine_handle<> handle) {
  if constexpr (logSchedulerOperations) {
    fmt::print("Enqueuing coroutine {:x}\n", (uintptr_t)handle.address());
  }

  // Use of moved-out Scheduler?
  BOOST_ASSERT(resumableActive_.capacity() > 0);
  resumableActive_.push_back(handle);
}

Scheduler::~Scheduler() = default;

Scheduler::Scheduler() {
  resumableActive_.reserve(16);
  resumableRunning_.reserve(16);
}

void Scheduler::cancel(std::coroutine_handle<> handle) {
  if constexpr (logSchedulerOperations) {
    fmt::print("Cancelling coroutine {:x}\n", (uintptr_t)handle.address());
  }

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
