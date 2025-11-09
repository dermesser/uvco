// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>
#include <uv.h>

#include "uvco/async_work.h"
#include "uvco/exception.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/promise.h"

#include <functional>

namespace uvco {

namespace {

class AsyncWorkAwaiter_ {
public:
  explicit AsyncWorkAwaiter_(std::function<void()> function)
      : work_{}, function_{std::move(function)} {
    setRequestData(&work_, this);
  }
  AsyncWorkAwaiter_(const AsyncWorkAwaiter_ &) = delete;
  AsyncWorkAwaiter_(AsyncWorkAwaiter_ &&) = delete;
  AsyncWorkAwaiter_ &operator=(const AsyncWorkAwaiter_ &) = delete;
  AsyncWorkAwaiter_ &operator=(AsyncWorkAwaiter_ &&) = delete;
  ~AsyncWorkAwaiter_() {
    uv_cancel((uv_req_t *)&work_);
    resetRequestData(&work_);
  }

  static void onDoWork(uv_work_t *work) {
    auto *awaiter = getRequestDataOrNull<AsyncWorkAwaiter_>(work);
    if (awaiter == nullptr) {
      // cancelled
      return;
    }
    awaiter->function_();
  }

  static void onWorkDone(uv_work_t *work, uv_status status) {
    if (status == UV_ECANCELED) {
      BOOST_ASSERT(requestDataIsNull(work));
      // Work was cancelled; do not resume the coroutine.
      return;
    }
    auto *awaiter = getRequestDataOrNull<AsyncWorkAwaiter_>(work);
    if (awaiter == nullptr) {
      // cancelled
      return;
    }
    awaiter->status_ = status;
    awaiter->schedule();
  }

  uv_work_t &work() { return work_; }

  [[nodiscard]] bool await_ready() const noexcept {
    return status_.has_value();
  }

  bool await_suspend(std::coroutine_handle<> handle) {
    BOOST_ASSERT(!status_);
    BOOST_ASSERT(!handle_);
    BOOST_ASSERT_MSG(!handle_, "AsyncWorkAwaiter_ can only be awaited once");
    handle_ = handle;
    return true;
  }

  void await_resume() {
    BOOST_ASSERT(status_.has_value());
    if (status_.value() != 0) {
      throw UvcoException(status_.value(), "AsyncWorkAwaiter_ failed");
    }
  }

private:
  uv_work_t work_;
  std::function<void()> function_;
  std::optional<uv_status> status_;
  std::optional<std::coroutine_handle<>> handle_;

  void invoke() {
    BOOST_ASSERT(function_ != nullptr);
    function_();
  }

  void schedule() {
    if (handle_) {
      const auto handle = handle_.value();
      handle_ = std::nullopt;
      Loop::enqueue(handle);
    }
  }
};

} // namespace

Promise<void> innerSubmitWork(const Loop &loop, std::function<void()> work) {
  AsyncWorkAwaiter_ awaiter{std::move(work)};
  uv_queue_work(loop.uvloop(), &awaiter.work(), AsyncWorkAwaiter_::onDoWork,
                AsyncWorkAwaiter_::onWorkDone);
  co_await awaiter;
  co_return;
}

template <>
Promise<void> submitWork(const Loop &loop, std::function<void()> work) {
  std::optional<std::exception_ptr> result;
  // Erase return type and use generic submitWork().
  std::function<void()> agnosticWork = [&result, work = std::move(work)]() {
    try {
      work();
    } catch (...) {
      result = std::current_exception();
    }
  };
  co_await innerSubmitWork(loop, std::move(agnosticWork));
  if (result.has_value()) {
    std::rethrow_exception(result.value());
  }
  co_return;
}

} // namespace uvco
