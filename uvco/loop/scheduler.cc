// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <cstdint>
#include <fmt/core.h>
#include <uv.h>

#include "uvco/loop/scheduler.h"

#include <coroutine>

namespace uvco {

/// If set to true, always resume coroutines from the scheduler. Otherwise,
/// coroutines may be resumed upon suspension of another coroutine. This can
/// make control flow easier to understand and debug; coroutines being directly
/// resumed upon resumption of another one may result in deep and random stacks,
/// which is especially inconvenient when profiling code, as flamegraphs and
/// other aggregates will show samples spread across many different stacks.
static constexpr bool useSymmetricHandoff = true;

void Scheduler::runAll() {
  while (!resumable_.empty()) {
    const std::coroutine_handle<> next = getNextInner();
    BOOST_ASSERT(next != nullptr && !next.done());
    if constexpr (logSchedulerOperations) {
      fmt::print("Resuming coroutine {:x}\n", (uintptr_t)next.address());
    }
    next.resume();
  }
}

void Scheduler::close() { BOOST_ASSERT(resumable_.empty()); }

void Scheduler::enqueue(std::coroutine_handle<> handle) {
  if constexpr (logSchedulerOperations) {
    fmt::print("Enqueuing coroutine {:x}\n", (uintptr_t)handle.address());
  }

  // Use of moved-out Scheduler?
  resumable_.push_back(handle);
}

Scheduler::~Scheduler() = default;
Scheduler::Scheduler() = default;

void Scheduler::cancel(std::coroutine_handle<> handle) {
  BOOST_ASSERT(handle != nullptr);
  if constexpr (logSchedulerOperations) {
    fmt::print("Cancelling coroutine {:x}\n", (uintptr_t)handle.address());
  }

  for (auto &it : resumable_) {
    if (it == handle) {
      it = nullptr;
    }
  }
}

std::coroutine_handle<> Scheduler::getNext() {
  if constexpr (useSymmetricHandoff) {
    return getNextInner();
  } else {
    return std::noop_coroutine();
  }
}

std::coroutine_handle<> Scheduler::getNextInner() {
  std::coroutine_handle<> next{nullptr};
  while (!resumable_.empty() && (next == nullptr || next.done())) {
    next = resumable_.front();
    resumable_.pop_front();
  }
  if (next == nullptr || next.done()) {
    return std::noop_coroutine();
  }
  if constexpr (logSchedulerOperations) {
    fmt::print("Dequeuing coroutine {:x}, {} left\n", (uintptr_t)next.address(),
               resumable_.size());
  }
  return next;
}

} // namespace uvco
