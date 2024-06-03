// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include "promise/promise.h"

#include "exception.h"

#include <coroutine>
#include <cstdio>
#include <exception>
#include <uv.h>

namespace uvco {

bool Promise<void>::PromiseAwaiter_::await_suspend(
    std::coroutine_handle<> handle) const {
  BOOST_ASSERT(!core_.ready && !core_.exception);
  BOOST_ASSERT_MSG(!core_.willResume(),
                   "promise is already being waited on!\n");
  core_.setHandle(handle);
  return true;
}

bool Promise<void>::PromiseAwaiter_::await_ready() const {
  return core_.ready || core_.exception;
}

void Promise<void>::PromiseAwaiter_::await_resume() const {
  if (core_.exception) {
    std::rethrow_exception(core_.exception.value());
  }
  BOOST_ASSERT(core_.ready);
}

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

void Promise<void>::unwrap() {
  if (ready()) {
    if (core_->exception) {
      std::rethrow_exception(core_->exception.value());
    }
  } else {
    throw UvcoException(UV_EAGAIN, "unwrap called on unfulfilled promise");
  }
}

void Coroutine<void>::return_void() {
  core_->ready = true;
  core_->resume();
}

void Coroutine<void>::unhandled_exception() {
  core_->except(std::current_exception());
  core_->resume();
}

} // namespace uvco
