// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <uv.h>

#include "loop/scheduler.h"

#include <coroutine>

namespace uvco {

void Scheduler::runAll() {
  resumableActive_.swap(resumableRunning_);
  for (auto &coro : resumableRunning_) {
    coro.resume();
  }
  resumableRunning_.clear();
}

void Scheduler::close() { BOOST_ASSERT(resumableActive_.empty()); }

void Scheduler::enqueue(std::coroutine_handle<> handle) {
  // Use of moved-out Scheduler?
  BOOST_ASSERT(resumableActive_.capacity() != 0);

  if (run_mode_ == RunMode::Immediate) {
    handle.resume();
    return;
  }

  resumableActive_.push_back(handle);
}

void Scheduler::setUpLoop(uv_loop_t *loop) { uv_loop_set_data(loop, this); }

Scheduler::~Scheduler() = default;

} // namespace uvco
