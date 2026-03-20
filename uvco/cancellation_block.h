// Copyright (C) 2026 kimapr.  See LICENSE for specific terms.

#include "uvco/promise/promise_refcount.h"
#include "uvco/loop/loop.h"
#include "uvco/combinators.h"

#pragma once

namespace uvco {

/// This object prevents the coroutine in which it was created, as well as
/// the entire async "call stack" from being destroyed early.
///
/// This is useful when performing an operation that cannot be safely
/// cancelled, to ensure that all the objects it references continue to exist.
///
/// Constructing the object begins the block, destruction ends it. The object
/// can also be `co_awaited` to end the block without waiting for the next loop
/// cycle.
class CancellationBlock {
  CoroutineHandle target;

  struct InitialSuspendAwaiter {
    std::coroutine_handle<> *coro;

    bool await_ready() { return false; }
    bool await_suspend(std::coroutine_handle<> scoro) {
      *coro = scoro;
      return true;
    }
    void await_resume() {}
  };
public:
  CancellationBlock() {
    target = RunningCoroutine::get();
    PromiseRefcounter_::acquireRecursive(target.ref);
  }
  CancellationBlock(const CancellationBlock &) = delete;
  CancellationBlock(CancellationBlock &&) = delete;
  CancellationBlock &operator=(const CancellationBlock &) = delete;
  CancellationBlock &operator=(CancellationBlock &&) = delete;

  bool await_ready() { return false; }
  bool await_suspend(std::coroutine_handle<>) {
    auto tg = target;
    target = {};
    PromiseRefcounter_::releaseRecursive(tg.ref);
    if (tg.ref->uses()) return false;
    return true;
  }
  void await_resume() {}

  ~CancellationBlock() {
    if (!target) return;
    detach(([](auto target) -> Promise<void>{
      co_await yield();
      auto tg = target;
      PromiseRefcounter_::releaseRecursive(tg.ref);
    })(std::move(target)));
  }
};

} // namespace uvco
