// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include "promise/promise_core.h"
#include "exception.h"
#include "loop/loop.h"

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
  resume_ = handle;
  state_ = PromiseState::waitedOn;
}

bool PromiseCore<void>::willResume() { return resume_.has_value(); }

void PromiseCore<void>::resume() {
  if (resume_) {
    BOOST_ASSERT(state_ == PromiseState::waitedOn);
    auto resumeHandle = *resume_;
    resume_.reset();
    state_ = PromiseState::running;
    Loop::enqueue(resumeHandle);
  } else {
    // If a coroutine returned immediately, or nobody is co_awaiting the result.
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

void PromiseCore<void>::except(std::exception_ptr exc) {
  BOOST_ASSERT(state_ == PromiseState::init ||
               state_ == PromiseState::waitedOn);
  exception = std::move(exc);
  ready = true;
}

void PromiseCore<void>::cancel() {
  if (state_ == PromiseState::waitedOn) {
    BOOST_ASSERT(!exception);
    if (!exception) {
      try {
        throw UvcoException(UV_ECANCELED, "Promise cancelled");
      } catch (...) {
        exception = std::current_exception();
      }
    }
    resume();
  }
}
} // namespace uvco
