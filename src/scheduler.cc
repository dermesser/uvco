// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include "scheduler.h"

namespace uvco {

void Scheduler::runAll() {
  for (auto coro : resumable_) {
    coro.resume();
  }
  resumable_.clear();
  uv_prepare_stop(&prepare_);
}

Promise<void> Scheduler::close(const uv_loop_t *loop) {
  return ((Scheduler *)loop->data)->close();
}

Promise<void> Scheduler::close() { co_await closeHandle(&prepare_); }

void Scheduler::enqueue(std::coroutine_handle<> handle) {
  // Use of moved-out Scheduler?
  BOOST_ASSERT(resumable_.capacity() != 0);

  if (run_mode_ == RunMode::Immediate) {
    handle.resume();
    return;
  }

  if (resumable_.empty()) {
    uv_prepare_start(&prepare_, onprepare);
  }
  resumable_.push_back(handle);
}

void Scheduler::setUpLoop(uv_loop_t *loop) {
  loop->data = this;
  uv_prepare_init(loop, &prepare_);
}

Scheduler::~Scheduler() {
  // Trick: saves us from having to explicitly define move
  // assignment/constructors.
  if (resumable_.capacity() != 0) {
    uv_prepare_stop(&prepare_);
  }
}

} // namespace uvco
