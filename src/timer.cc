// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <uv.h>

#include "close.h"
#include "promise.h"
#include "timer.h"

#include <cstdint>

namespace uvco {

namespace {

void onTimerDone(uv_timer_t *handle);

class TimerAwaiter {
public:
  TimerAwaiter(uv_loop_t *loop, uint64_t millis) : handle_{} {
    uv_timer_init(loop, &handle_);
    handle_.data = this;
    uv_timer_start(&handle_, onTimerDone, millis, 0);
  }
  ~TimerAwaiter() { BOOST_ASSERT(closed_); }

  Promise<void> close() {
    co_await closeHandle(&handle_);
    closed_ = true;
  }

  bool await_ready() { return isReady(); }
  bool await_suspend(std::coroutine_handle<> handle) {
    resume_ = handle;
    return true;
  }
  void await_resume() {}

  bool isReady() {
    uint64_t due = uv_timer_get_due_in(&handle_);
    return due == 0;
  }
  void stop() { uv_timer_stop(&handle_); }
  void resume() {
    if (resume_) {
      resume_->resume();
    }
  }

private:
  uv_timer_t handle_;
  std::optional<std::coroutine_handle<>> resume_;
  bool closed_ = false;
};

void onTimerDone(uv_timer_t *handle) {
  auto *awaiter = (TimerAwaiter *)handle->data;
  awaiter->stop();
  awaiter->resume();
}

} // namespace

Promise<void> wait(uv_loop_t *loop, uint64_t millis) {
  TimerAwaiter awaiter{loop, millis};
  co_await awaiter;
  co_await awaiter.close();
  co_return;
}

} // namespace uvco
