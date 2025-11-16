// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <fmt/core.h>
#include <uv.h>

#include <boost/assert.hpp>

#include "uvco/close.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"
#include "uvco/timer.h"

#include <coroutine>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace uvco {

void onSingleTimerDone(uv_timer_t *handle);
void onMultiTimerFired(uv_timer_t *handle);

class TimerAwaiter {
public:
  TimerAwaiter(const TimerAwaiter &) = delete;
  TimerAwaiter(TimerAwaiter &&other) noexcept
      : timer_{std::move(other.timer_)}, handle_{other.handle_},
        stopped_{other.stopped_} {
    setData(timer_.get(), this);
    other.closed_ = true;
  }
  TimerAwaiter &operator=(const TimerAwaiter &) = delete;
  TimerAwaiter &operator=(TimerAwaiter &&other) noexcept {
    timer_ = std::move(other.timer_);
    handle_ = other.handle_;
    stopped_ = other.stopped_;
    setData(timer_.get(), this);
    other.closed_ = true;
    return *this;
  }
  TimerAwaiter(const Loop &loop, uint64_t millis, bool repeating = false)
      : timer_{std::make_unique<uv_timer_t>()} {
    uv_timer_init(loop.uvloop(), timer_.get());
    setData(timer_.get(), this);
    if (repeating) {
      uv_timer_start(timer_.get(), onMultiTimerFired, millis, millis);
    } else {
      uv_timer_start(timer_.get(), onSingleTimerDone, millis, 0);
    }
  }
  ~TimerAwaiter() {
    stop();
    if (!closed_ && timer_) {
      closeHandle(timer_.release());
    }
  }

  Promise<void> close() {
    if (!timer_ || closed_) {
      co_return;
    }
    closed_ = true;
    // If the timer is already closing, we don't need to close it again.
    // This would cause an error in libuv.
    if (isClosed(timer_.get())) {
      co_return;
    }
    stop();
    co_await closeHandle(timer_.get());
    timer_.reset();
  }

  bool await_ready() { return isReady(); }
  bool await_suspend(std::coroutine_handle<> handle) {
    handle_ = handle;
    return true;
  }
  [[nodiscard]] bool await_resume() const { return !stopped_; }

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
    if (handle_) {
      auto handle = *handle_;
      handle_.reset();
      Loop::enqueue(handle);
    }
  }

private:
  std::unique_ptr<uv_timer_t> timer_;
  std::optional<std::coroutine_handle<>> handle_;
  bool closed_ = false;
  bool stopped_ = false;
};

void onSingleTimerDone(uv_timer_t *handle) {
  auto *awaiter = getData<TimerAwaiter>(handle);
  awaiter->stop();
  awaiter->resume();
}

void onMultiTimerFired(uv_timer_t *handle) {
  auto *awaiter = getData<TimerAwaiter>(handle);
  awaiter->resume();
}

Promise<void> sleep(const Loop &loop, uint64_t millis) {
  TimerAwaiter awaiter{loop, millis};
  BOOST_VERIFY(!co_await awaiter);
  co_await awaiter.close();
  co_return;
}

/// Non-movable, non-copyable: because the awaiter is called by a callback.
class TickerImpl : public Ticker {
public:
  TickerImpl(const TickerImpl &) = delete;
  TickerImpl(TickerImpl &&) = default;
  TickerImpl &operator=(const TickerImpl &) = delete;
  TickerImpl &operator=(TickerImpl &&) = default;
  TickerImpl(std::unique_ptr<TimerAwaiter> awaiter, uint64_t max)
      : awaiter_{std::move(awaiter)}, count_max_{max} {}
  ~TickerImpl() override = default;

  MultiPromise<uint64_t> ticker() override;
  Promise<void> close() override;

private:
  std::unique_ptr<TimerAwaiter> awaiter_;
  uint64_t count_max_;
  bool stopped_ = false;
  bool running_ = false;
};

MultiPromise<uint64_t> TickerImpl::ticker() {
  FlagGuard guard(running_);

  uint64_t counter = 0;
  while (!stopped_ && (count_max_ == 0 || counter < count_max_)) {
    // Resumed from onMultiTimerFired():
    if (co_await *awaiter_) {
      if (stopped_) {
        break;
      }
      co_yield counter;
      ++counter;
    }
  }
  // Clean up if not stopped manually from stop().
  if (!stopped_) {
    // Need to close timer so that libuv isn't blocked by the active handle on
    // the loop.
    co_await close();
  }
}

Promise<void> TickerImpl::close() {
  stopped_ = true;
  // The stopped awaiter will yield a false event, and then break out of the
  // loop (ticker() method).
  awaiter_->resume();
  awaiter_->stop();
  co_await awaiter_->close();
}

std::unique_ptr<Ticker> tick(const Loop &loop, uint64_t millis,
                             uint64_t count) {
  auto awaiter = std::make_unique<TimerAwaiter>(loop, millis, true);
  return std::make_unique<TickerImpl>(std::move(awaiter), count);
}

} // namespace uvco
