// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include "uvco/combinators.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"

#include <coroutine>

namespace uvco {

namespace {

struct YieldAwaiter_ {
  [[nodiscard]] static bool await_ready() noexcept { return false; }
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

} // namespace uvco
