// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <uv.h>

#include "close.h"
#include "promise/promise.h"
#include "scheduler.h"

#include <coroutine>

namespace uvco {

void Scheduler::runAll() {
  // TODO: optimize this by keeping two lists.
  std::swap(resumableActive_, resumableRunning_);
  for (auto &coro : resumableRunning_) {
    coro.resume();
  }
  resumableRunning_.clear();
  uv_prepare_stop(&prepare_);
}

Promise<void> Scheduler::close() {
  BOOST_ASSERT(resumableActive_.empty());
  co_await closeHandle(&prepare_);
}

void Scheduler::enqueue(std::coroutine_handle<> handle) {
  // Use of moved-out Scheduler?
  BOOST_ASSERT(resumableActive_.capacity() != 0);

  if (run_mode_ == RunMode::Immediate) {
    handle.resume();
    return;
  }

  if (resumableActive_.empty()) {
    uv_prepare_start(&prepare_, onPrepare);
  }
  resumableActive_.push_back(handle);
}

void Scheduler::setUpLoop(uv_loop_t *loop) {
  uv_loop_set_data(loop, this);
  uv_prepare_init(loop, &prepare_);
}

Scheduler::~Scheduler() {
  // Trick: saves us from having to explicitly define move
  // assignment/constructors.
  if (resumableActive_.capacity() != 0) {
    uv_prepare_stop(&prepare_);
  }
}

} // namespace uvco
