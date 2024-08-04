// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <coroutine>
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
  ~AsyncWorkAwaiter_() = default;

  static void onDoWork(uv_work_t *work) {
    auto *awaiter = getRequestData<AsyncWorkAwaiter_>(work);
    awaiter->function_();
  }

  static void onWorkDone(uv_work_t *work, uv_status status) {
    auto *awaiter = getRequestData<AsyncWorkAwaiter_>(work);
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

Promise<void> submitWork(const Loop &loop, std::function<void()> work) {
  AsyncWorkAwaiter_ awaiter{work};
  uv_queue_work(loop.uvloop(), &awaiter.work(), AsyncWorkAwaiter_::onDoWork,
                AsyncWorkAwaiter_::onWorkDone);
  co_await awaiter;
  co_return;
}

} // namespace uvco
