// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <coroutine>
#include <uv.h>

#include "exception.h"
#include "internal/internal_utils.h"
#include "loop/loop.h"
#include "loop/scheduler.h"

#include <cstdio>
#include <fmt/core.h>
#include <memory>

namespace uvco {

Loop::Loop(Scheduler::RunMode mode)
    : loop_{std::make_unique<uv_loop_t>()},
      scheduler_{std::make_unique<Scheduler>(mode)} {

  if (defaultLoop != nullptr) {
    throw UvcoException(UV_EBUSY,
                        "Loop::Loop(): only one loop can be created.");
  }

  uv_loop_init(loop_.get());
  uv_loop_set_data(loop_.get(), scheduler_.get());
  scheduler_->setUpLoop(loop_.get());
  defaultLoop = this;
}

Loop::~Loop() {
  scheduler_->close();

  // Run loop again so that all handles have been closed.
  // A single turn is enough.
  runOne();

  // Now run all scheduled handles; especially the scheduler_->close()
  // coroutine.
  scheduler_->runAll();

  const uv_status status = uv_loop_close(loop_.get());
  if (0 != status) {
    fmt::print(stderr,
               "Loop::~Loop(): uv_loop_close() failed; there were "
               "still resources on the loop: {}\n",
               uv_strerror(status));
  }
  defaultLoop = nullptr;
}

void Loop::runOne() { uv_run(loop_.get(), UV_RUN_ONCE); }

void Loop::run() {
  while (!scheduler_->empty() || uv_loop_alive(loop_.get()) != 0) {
    runOne();
    // Run any left-over coroutines, and check if they schedule callbacks.
    scheduler_->runAll();
  }
}

uv_loop_t *Loop::uvloop() const { return loop_.get(); }

Loop *Loop::defaultLoop = nullptr;

Scheduler &Loop::currentScheduler() {
  if (defaultLoop == nullptr) {
    throw UvcoException(UV_EINVAL, "Loop::getDefaultLoop(): no loop created.");
  }
  return *defaultLoop->scheduler_;
}

void Loop::enqueue(std::coroutine_handle<> handle) {
  currentScheduler().enqueue(handle);
  // If any handles are present, ensure that uv_run returns from waiting for I/O
  // soon.
  uv_stop(defaultLoop->uvloop());
}

} // namespace uvco
