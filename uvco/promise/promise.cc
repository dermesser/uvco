// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <uv.h>

#include "uvco/exception.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/promise/promise.h"
#include "uvco/promise/promise_core.h"

#include <coroutine>
#include <exception>

namespace uvco {

Promise<void>::PromiseAwaiter_::PromiseAwaiter_(PromiseCore<void> &core)
    : core_{core} {}

Promise<void>::PromiseAwaiter_ Promise<void>::operator co_await() const {
  return PromiseAwaiter_{coroutine_->core()};
}

bool Promise<void>::PromiseAwaiter_::await_suspend(
    std::coroutine_handle<> handle) const {
  BOOST_ASSERT(!core_.ready_ && !core_.exception_);
  BOOST_ASSERT_MSG(!core_.willResume(),
                   "promise is already being waited on!\n");
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
}

Promise<void>::Promise() = default;
Promise<void>::Promise(Coroutine<void> *coroutine) : coroutine_{coroutine} {}
Promise<void>::Promise(Promise<void> &&other) noexcept
    : coroutine_{other.coroutine_} {
  other.coroutine_ = nullptr;
}

Promise<void> &Promise<void>::operator=(const Promise<void> &other) {
  if (this == &other) {
    return *this;
  }
  if (coroutine_ != nullptr) {
    coroutine_->dropRef();
  }
  coroutine_ = other.coroutine_;
  coroutine_->addRef();
  return *this;
}

Promise<void> &Promise<void>::operator=(Promise<void> &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  if (coroutine_ != nullptr) {
    coroutine_->dropRef();
  }
  coroutine_ = other.coroutine_;
  other.coroutine_ = nullptr;
  return *this;
}

Promise<void>::Promise(const Promise<void> &other)
    : coroutine_{other.coroutine_} {}

Promise<void>::~Promise() {
  if (coroutine_ != nullptr) {
    coroutine_->dropRef();
  }
}

bool Promise<void>::ready() const { return coroutine_->core().ready_; }

void Promise<void>::unwrap() {
  if (ready()) {
    if (coroutine_->core().exception_) {
      std::rethrow_exception(coroutine_->core().exception_.value());
    }
  } else {
    throw UvcoException(UV_EAGAIN, "unwrap called on unfulfilled promise");
  }
}

PromiseHandle<void> Promise<void>::handle() {
  return PromiseHandle<void>{&coroutine_->core()};
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
