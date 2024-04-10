// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <coroutine>
#include <uv.h>

#include "exception.h"
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
  // Schedule closing of scheduler, which deletes the prepare handle.
  // We can't await the promise in the destructor. However, it will
  // schedule a single callback on the loop, which will enqueue the
  // scheduler_->close() coroutine. One runAll() call will then run it.
  scheduler_->close();

  // Run loop again so that all handles have been closed.
  // A single turn is enough.
  runOne();

  // Now run all scheduled handles; especially the scheduler_->close()
  // coroutine.
  scheduler_->runAll();

  if (0 != uv_loop_close(loop_.get())) {
    fmt::print(stderr, "Loop::~Loop(): uv_loop_close() failed; there were "
                       "still resources on the loop.\n");
  }
  defaultLoop = nullptr;
}

void Loop::runOne() { uv_run(loop_.get(), UV_RUN_ONCE); }
void Loop::run() {
  do {
    runOne();
    // Run any left-over coroutines, and check if they schedule callbacks.
    scheduler_->runAll();
  } while (!scheduler_->empty() || uv_loop_alive(loop_.get()) != 0);
}
uv_loop_t *Loop::uvloop() const { return loop_.get(); }

Loop::operator uv_loop_t *() const { return loop_.get(); }

Loop *Loop::defaultLoop = nullptr;

Scheduler &Loop::currentScheduler() {
  if (defaultLoop == nullptr) {
    throw UvcoException(UV_EINVAL, "Loop::getDefaultLoop(): no loop created.");
  }
  return *defaultLoop->scheduler_;
}

void Loop::enqueue(std::coroutine_handle<> handle) {
  currentScheduler().enqueue(handle);
}

} // namespace uvco
