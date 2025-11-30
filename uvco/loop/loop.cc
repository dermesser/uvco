// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <fmt/format.h>
#include <uv.h>

#include "uvco/exception.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/loop/loop.h"
#include "uvco/loop/scheduler.h"

#include <coroutine>

namespace uvco {

namespace {

void handleWalkCallbackForDebug(uv_handle_t *handle, void * /* arg */) {
  fmt::print("A handle of type {} is still active\n",
             uv_handle_type_name(handle->type));
}

} // namespace

Loop::Loop() : loop_{} {

  if (defaultLoop != nullptr) {
    throw UvcoException(UV_EBUSY,
                        "Loop::Loop(): only one loop can be created.");
  }

  uv_loop_init(&loop_);
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
    // Walk the loop and print all handles that are still active.
    uv_walk(&loop_, handleWalkCallbackForDebug, nullptr);
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

  if constexpr (logSchedulerOperations) {
    fmt::print("exiting loop due to scheduler empty? {} uv_loop_alive? {}\n",
               scheduler_.empty() ? "yes" : "no",
               uv_loop_alive(&loop_) != 0 ? "yes" : "no");
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
  currentScheduler().enqueue(handle);
  // If any handles are present, ensure that uv_run returns from waiting for I/O
  // soon.
  uv_stop(defaultLoop->uvloop());
}

void Loop::cancel(std::coroutine_handle<> handle) {
  currentScheduler().cancel(handle);
}

std::coroutine_handle<> Loop::getNext() { return currentScheduler().getNext(); }

} // namespace uvco
