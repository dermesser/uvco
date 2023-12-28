// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include "promise.h"

namespace uvco {

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
