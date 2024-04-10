// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include "promise/promise_core.h"
#include "loop/loop.h"
#include "loop/scheduler.h"

#include <coroutine>
#include <cstdio>
#include <exception>
#include <fmt/core.h>
#include <utility>

namespace uvco {

void PromiseCore<void>::set_handle(std::coroutine_handle<> h) {
  BOOST_ASSERT(state_ == PromiseState::init);
  resume_ = h;
  state_ = PromiseState::waitedOn;
}

bool PromiseCore<void>::willResume() { return resume_.has_value(); }

void PromiseCore<void>::resume() {
  if (resume_) {
    BOOST_ASSERT(state_ == PromiseState::waitedOn ||
                 state_ == PromiseState::exception);
    auto resumeHandle = *resume_;
    resume_.reset();
    state_ = PromiseState::running;
    // Directly jump to the awaiting coroutine.
    resumeHandle.resume();
  } else {
    // If a coroutine returned immediately, or nobody is f
    // :co_awaitis for results.
  }
  state_ = PromiseState::finished;
}

PromiseCore<void>::~PromiseCore() {
  BOOST_ASSERT(state_ != PromiseState::running);
  if (state_ == PromiseState::init) {
    fmt::print(stderr, "void Promise not finished\n");
  }
  if (resume_) {
    fmt::print(stderr, "resumable coroutine destroyed\n");
    resume_->destroy();
  }
}

void PromiseCore<void>::immediateFulfill() {
  ready = true;
  state_ = PromiseState::finished;
}

void PromiseCore<void>::except(std::exception_ptr exc) {
  BOOST_ASSERT(state_ == PromiseState::init ||
               state_ == PromiseState::waitedOn);
  exception_ = std::move(exc);
  ready = true;
  state_ = PromiseState::exception;
}

} // namespace uvco
