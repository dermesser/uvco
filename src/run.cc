// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include "loop/loop.h"
#include "promise/multipromise.h"
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

Promise<void> yield() { co_await YieldAwaiter_{}; }

MultiPromise<unsigned> yield(unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    co_yield i;
  }
}

void runLoop(Loop &loop) { loop.run(); }

} // namespace uvco
