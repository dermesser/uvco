// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include "promise.h"

namespace uvco {

void PromiseCore<void>::set_resume(std::coroutine_handle<> h) {
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
    resumeHandle.resume();
  } else {
    // If a coroutine returned immediately, or nobody co_awaited for results.
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

void PromiseCore<void>::except(std::exception_ptr e) {
  exception_ = e;
  ready = true;
  state_ = PromiseState::exception;
}

} // namespace uvco
