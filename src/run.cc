// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include "loop/loop.h"
#include "promise/promise.h"
#include <coroutine>

namespace uvco {

namespace {

struct YieldAwaiter_ {
  [[nodiscard]] bool await_ready() const noexcept { return false; }
  bool await_suspend(std::coroutine_handle<> handle) noexcept {
    Loop::enqueue(handle);
    return true;
  }
  void await_resume() const noexcept {}
};

} // namespace

Promise<void> yield(const Loop &loop) { co_await YieldAwaiter_{}; }

void runLoop(Loop &loop) { loop.run(); }

} // namespace uvco
