// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <uv.h>

#include "uvco/exception.h"
#include "uvco/promise/promise.h"
#include "uvco/promise/promise_core.h"

#include <coroutine>
#include <exception>

namespace uvco {

Promise<void>::PromiseAwaiter_::PromiseAwaiter_(PromiseCore<void> &core)
    : core_{core} {}

Promise<void>::PromiseAwaiter_ Promise<void>::operator co_await() const {
  return PromiseAwaiter_{*core_};
}

bool Promise<void>::PromiseAwaiter_::await_suspend(
    std::coroutine_handle<> handle) const {
  BOOST_ASSERT(!core_.ready_ && !core_.exception_);
  BOOST_ASSERT_MSG(!core_.isAwaited(), "promise is already being waited on!\n");
  core_.setHandle(handle);
  return true;
}

bool Promise<void>::PromiseAwaiter_::await_ready() const {
  return core_.ready_ || core_.exception_;
}

void Promise<void>::PromiseAwaiter_::await_resume() const {
  if (core_.stale()) {
    throw UvcoException(
        "co_await called on previously finished promise (void)");
  }
  if (core_.exception_) {
    std::rethrow_exception(core_.exception_.value());
  }
  BOOST_ASSERT(core_.ready_);
  core_.ready_ = false;
}

Promise<void>::Promise(PromiseCore<void> &core) : core_{&core} {}
Promise<void>::Promise(Promise<void> &&other) noexcept : core_{other.core_} {
  other.core_ = nullptr;
}

Promise<void> &Promise<void>::operator=(Promise<void> &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  core_->destroyCoroutine();
  core_ = other.core_;
  other.core_ = nullptr;
  return *this;
}

Promise<void>::~Promise() {
  if (core_ != nullptr) {
    core_->destroyCoroutine();
  }
}

bool Promise<void>::ready() const { return core_->ready_; }

void Promise<void>::unwrap() {
  if (ready()) {
    if (core_->exception_) {
      std::rethrow_exception(core_->exception_.value());
    }
  } else {
    throw UvcoException(UV_EAGAIN, "unwrap called on unfulfilled promise");
  }
}

void Coroutine<void>::return_void() {
  core_.ready_ = true;
  core_.resume();
}

void Coroutine<void>::unhandled_exception() {
  core_.except(std::current_exception());
  core_.resume();
}

} // namespace uvco
