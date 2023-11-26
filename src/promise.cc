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
    BOOST_ASSERT(state_ == PromiseState::waitedOn);
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
    resume_->destroy();
  }
}

void PromiseCore<void>::immediateFulfill() {
  ready = true;
  state_ = PromiseState::finished;
}

Promise<void> Promise<void>::immediate() {
  Promise<void> imm{};
  imm.core_->immediateFulfill();
  return imm;
}

void Promise<void>::return_void() {
  core_->ready = true;
  core_->resume();
}

void Promise<void>::unhandled_exception() {
  std::rethrow_exception(std::current_exception());
}

bool Promise<void>::PromiseAwaiter_::await_suspend(
    std::coroutine_handle<> handle) {
  BOOST_ASSERT_MSG(!core_->willResume(),
                   "promise is already being waited on!\n");
  core_->set_resume(handle);
  return true;
}

bool Promise<void>::PromiseAwaiter_::await_ready() const {
  return core_->ready;
}

void Promise<void>::PromiseAwaiter_::await_resume() {}

Promise<void>::Promise(Promise<void> &&other) noexcept : core_{other.core_} {
  other.core_ = nullptr;
}

Promise<void> &Promise<void>::operator=(const Promise<void> &other) {
  if (this == &other) {
    return *this;
  }
  if (core_ != nullptr) {
    core_->delRef();
  }
  core_ = other.core_->addRef();
  return *this;
}

Promise<void> &Promise<void>::operator=(Promise<void> &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  if (core_ != nullptr) {
    core_->delRef();
  }
  core_ = other.core_;
  other.core_ = nullptr;
  return *this;
}

Promise<void>::Promise(const Promise<void> &other)
    : core_{other.core_->addRef()} {}

Promise<void>::~Promise() {
  if (core_ != nullptr) {
    core_->delRef();
  }
}
} // namespace uvco
