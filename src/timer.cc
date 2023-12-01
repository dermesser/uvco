// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <uv.h>

#include <boost/assert.hpp>

#include "close.h"
#include "internal_utils.h"
#include "promise.h"
#include "timer.h"

#include <coroutine>
#include <cstdint>
#include <optional>
#include <utility>

namespace uvco {

void onSingleTimerDone(uv_timer_t *handle);
void onMultiTimerFired(uv_timer_t *handle);

class TimerAwaiter {
public:
  TimerAwaiter(const TimerAwaiter &) = delete;
  TimerAwaiter(TimerAwaiter &&other) noexcept
      : timer_{std::move(other.timer_)}, resume_{other.resume_},
        stopped_{other.stopped_} {
    timer_->data = this;
    other.closed_ = true;
  }
  TimerAwaiter &operator=(const TimerAwaiter &) = delete;
  TimerAwaiter &operator=(TimerAwaiter &&other) noexcept {
    timer_ = std::move(other.timer_);
    resume_ = other.resume_;
    stopped_ = other.stopped_;
    timer_->data = this;
    other.closed_ = true;
    return *this;
  }
  TimerAwaiter(uv_loop_t *loop, uint64_t millis, bool repeating = false)
      : timer_{std::make_unique<uv_timer_t>()} {
    uv_timer_init(loop, timer_.get());
    timer_->data = this;
    if (repeating) {
      uv_timer_start(timer_.get(), onMultiTimerFired, millis, millis);
    } else {
      uv_timer_start(timer_.get(), onSingleTimerDone, millis, 0);
    }
  }
  ~TimerAwaiter() {
    BOOST_ASSERT_MSG(
        closed_, "Timer still active: please close explicitly using close()");
  }

  Promise<void> close() {
    if (!timer_) {
      co_return;
    }
    stop();
    co_await closeHandle(timer_.get());
    closed_ = true;
    timer_.reset();
  }

  bool await_ready() { return isReady(); }
  bool await_suspend(std::coroutine_handle<> handle) {
    resume_ = handle;
    return true;
  }
  bool await_resume() { return !stopped_; }

  bool isReady() {
    uint64_t due = uv_timer_get_due_in(timer_.get());
    return due == 0;
  }
  void stop() {
    if (!stopped_) {
      uv_timer_stop(timer_.get());
      stopped_ = true;
    }
  }
  void resume() {
    if (resume_) {
      auto resume = *resume_;
      resume_.reset();
      resume.resume();
    }
  }

private:
  std::unique_ptr<uv_timer_t> timer_;
  std::optional<std::coroutine_handle<>> resume_;
  bool closed_ = false;
  bool stopped_ = false;
};

void onSingleTimerDone(uv_timer_t *handle) {
  auto *awaiter = (TimerAwaiter *)handle->data;
  awaiter->stop();
  awaiter->resume();
}

void onMultiTimerFired(uv_timer_t *handle) {
  auto *awaiter = (TimerAwaiter *)handle->data;
  awaiter->resume();
}

Promise<void> wait(uv_loop_t *loop, uint64_t millis) {
  TimerAwaiter awaiter{loop, millis};
  co_await awaiter;
  co_await awaiter.close();
  co_return;
}

class TickerImpl : public Ticker {
public:
  ~TickerImpl() override = default;
  explicit TickerImpl(TimerAwaiter awaiter, uint64_t max)
      : awaiter_{std::move(awaiter)}, count_max_{max} {}

  MultiPromise<uint64_t> ticker() override;
  Promise<void> close() override;

private:
  TimerAwaiter awaiter_;
  uint64_t count_max_;
  bool stopped_ = false;
  bool running_ = false;
};

MultiPromise<uint64_t> TickerImpl::ticker() {
  FlagGuard guard(running_);

  uint64_t counter = 0;
  while (!stopped_ && (count_max_ == 0 || counter < count_max_)) {
    // Resumed from onMultiTimerFired():
    if (co_await awaiter_) {
      co_yield std::move(counter);
      ++counter;
    }
  }
  // Clean up if not stopped manually from stop().
  if (!stopped_) {
    stopped_ = true;
    awaiter_.stop();
    co_await awaiter_.close();
  }
}

Promise<void> TickerImpl::close() {
  stopped_ = true;
  // The stopped awaiter will yield a false event, and then break out of the
  // loop (ticker() method).
  awaiter_.resume();
  awaiter_.stop();
  co_await awaiter_.close();
}

std::unique_ptr<Ticker> tick(uv_loop_t *loop, uint64_t millis, uint64_t count) {
  TimerAwaiter awaiter{loop, millis, true};
  return std::make_unique<TickerImpl>(std::move(awaiter), count);
}

} // namespace uvco
