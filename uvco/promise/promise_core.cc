// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include "uvco/promise/promise_core.h"
#include "uvco/exception.h"
#include "uvco/loop/loop.h"

#include <fmt/core.h>
#include <uv.h>

#include <coroutine>
#include <exception>
#include <utility>

namespace uvco {

void PromiseCore<void>::setHandle(std::coroutine_handle<> handle) {
  if (state_ != PromiseState::init) {
    throw UvcoException("PromiseCore is already awaited or has finished");
  }
  BOOST_ASSERT(state_ == PromiseState::init);
  waitingHandle_ = handle;
  state_ = PromiseState::waitedOn;
}

bool PromiseCore<void>::isAwaited() const { return waitingHandle_ != nullptr; }
bool PromiseCore<void>::ready() const { return exception_ || ready_; }
bool PromiseCore<void>::stale() const {
  return state_ == PromiseState::finished && !ready();
}

void PromiseCore<void>::resume() {
  if (waitingHandle_) {
    BOOST_ASSERT(state_ == PromiseState::waitedOn);
    state_ = PromiseState::resuming;
    auto waitingHandle = waitingHandle_;
    waitingHandle_ = nullptr;
    Loop::enqueue(waitingHandle);
  } else {
    // If a coroutine returned immediately, or nobody is co_awaiting the result.
  }
  state_ = PromiseState::finished;
}

PromiseCore<void>::~PromiseCore() {
  BOOST_ASSERT(state_ != PromiseState::resuming);
  if (state_ == PromiseState::init) {
    fmt::print(stderr,
               "Promise<void> not finished (dropped Promise by accident?)\n");
  }
  if (waitingHandle_) {
    fmt::print(stderr, "resumable coroutine destroyed\n");
  }
}

void PromiseCore<void>::except(std::exception_ptr exc) {
  BOOST_ASSERT(state_ == PromiseState::init ||
               state_ == PromiseState::waitedOn);
  exception_ = std::move(exc);
  ready_ = true;
}

void PromiseCore<void>::resetHandle() {
  BOOST_ASSERT((state_ == PromiseState::waitedOn && waitingHandle_) ||
               (state_ == PromiseState::finished && !waitingHandle_));
  waitingHandle_ = nullptr;
  if (state_ == PromiseState::waitedOn) {
    state_ = PromiseState::init;
  }
}

void PromiseCore<void>::setRunning(std::coroutine_handle<> handle) {
  coroutine_ = handle;
}
} // namespace uvco
