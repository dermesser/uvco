// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <coroutine>
#include <uv.h>

#include "uvco/exception.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/loop/loop.h"
#include "uvco/loop/scheduler.h"

#include <cstdio>
#include <fmt/core.h>
#include <memory>

namespace uvco {

Loop::Loop() {
  if (defaultLoop != nullptr) {
    throw UvcoException(UV_EBUSY,
                        "Loop::Loop(): only one loop can be created.");
  }

  uv_loop_init(&loop_);
  uv_loop_set_data(&loop_, &scheduler_);
  scheduler_.setUpLoop(&loop_);
  defaultLoop = this;
}

Loop::~Loop() {
  scheduler_.close();

  // Run loop again so that all handles are closed.
  // A single turn is enough.
  runOne();

  // Now run all scheduled handles
  scheduler_.runAll();

  const uv_status status = uv_loop_close(&loop_);
  if (0 != status) {
    fmt::print(stderr,
               "Loop::~Loop(): uv_loop_close() failed; there were "
               "still resources on the loop: {}\n",
               uv_strerror(status));
  }
  defaultLoop = nullptr;
}

void Loop::runOne() { uv_run(&loop_, UV_RUN_ONCE); }

void Loop::run() {
  while (!scheduler_.empty() || uv_loop_alive(&loop_) != 0) {
    runOne();
    // Run any left-over coroutines, and check if they schedule callbacks.
    scheduler_.runAll();
  }
}

uv_loop_t *Loop::uvloop() const { return &loop_; }

Loop *Loop::defaultLoop = nullptr;

Scheduler &Loop::currentScheduler() {
  if (defaultLoop == nullptr) {
    throw UvcoException(UV_EINVAL, "Loop::getDefaultLoop(): no loop created.");
  }
  return defaultLoop->scheduler_;
}

void Loop::enqueue(std::coroutine_handle<> handle) {
  currentScheduler().resume(handle);
  // If any handles are present, ensure that uv_run returns from waiting for I/O
  // soon.
  uv_stop(defaultLoop->uvloop());
}

void Loop::enqueueTask(std::coroutine_handle<> handle) {
  currentScheduler().startTask(handle);
  uv_stop(defaultLoop->uvloop());
}

} // namespace uvco
