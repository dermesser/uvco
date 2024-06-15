// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include "uvco/promise/promise_core.h"
#include "uvco/exception.h"
#include "uvco/loop/loop.h"

#include <fmt/core.h>
#include <uv.h>

#include <coroutine>
#include <cstdio>
#include <exception>
#include <utility>

namespace uvco {

void PromiseCore<void>::setHandle(std::coroutine_handle<> handle) {
  if (state_ != PromiseState::init) {
    throw UvcoException("PromiseCore is already awaited or has finished");
  }
  BOOST_ASSERT(state_ == PromiseState::init);
  handle_ = handle;
  state_ = PromiseState::waitedOn;
}

bool PromiseCore<void>::willResume() const { return handle_.has_value(); }
bool PromiseCore<void>::ready() const { return exception_ || ready_; }
bool PromiseCore<void>::stale() const {
  return state_ == PromiseState::finished && !ready();
}

void PromiseCore<void>::resume() {
  if (handle_) {
    BOOST_ASSERT(state_ == PromiseState::waitedOn);
    state_ = PromiseState::resuming;
    auto resumeHandle = *handle_;
    handle_.reset();
    Loop::enqueue(resumeHandle);
  } else {
    // If a coroutine returned immediately, or nobody is co_awaiting the result.
  }
  state_ = PromiseState::finished;
}

PromiseCore<void>::~PromiseCore() {
  BOOST_ASSERT(state_ != PromiseState::resuming);
  if (state_ == PromiseState::init) {
    fmt::print(stderr, "void Promise not finished\n");
  }
  if (handle_) {
    fmt::print(stderr, "resumable coroutine destroyed\n");
    handle_->destroy();
  }
}

void PromiseCore<void>::except(std::exception_ptr exc) {
  BOOST_ASSERT(state_ == PromiseState::init ||
               state_ == PromiseState::waitedOn);
  exception_ = std::move(exc);
  ready_ = true;
}

void PromiseCore<void>::cancel() {
  if (state_ == PromiseState::waitedOn) {
    BOOST_ASSERT(!exception_);
    if (!exception_) {
      exception_ = std::make_exception_ptr(
          UvcoException(UV_ECANCELED, "Promise cancelled"));
    }
    resume();
  }
}

void PromiseCore<void>::resetHandle() {
  BOOST_ASSERT((state_ == PromiseState::waitedOn && handle_) ||
               (state_ == PromiseState::finished && !handle_));
  handle_.reset();
  if (state_ == PromiseState::waitedOn) {
    state_ = PromiseState::init;
  }
}

} // namespace uvco
