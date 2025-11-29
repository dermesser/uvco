#include "uvco/combinators.h"
#include "uvco/pipe.h"
#include <benchmark/benchmark.h>
#include <gtest/gtest.h>

using namespace uvco;

namespace {

void bmPromiseAwait(benchmark::State &state) {
  auto setup = [&state](const Loop &) -> Promise<void> {
    for (auto _ : state) {
      co_await yield();
    }
    co_return;
  };

  runMain<void>(setup);
}

BENCHMARK(bmPromiseAwait);

struct YieldAwaiter_ {
  [[nodiscard]] static bool await_ready() noexcept { return false; }
  bool await_suspend(std::coroutine_handle<> handle) noexcept {
    Loop::enqueue(handle);
    return true;
  }
  [[nodiscard]] int await_resume() const noexcept { return 1; }
};

Promise<int> yieldInt() { co_return (co_await YieldAwaiter_{}); }

void bmPromiseAwaitIntFromCustomAwaiter(benchmark::State &state) {
  auto setup = [&state](const Loop &) -> Promise<void> {
    for (auto _ : state) {
      co_await yieldInt();
    }
    co_return;
  };

  runMain<void>(setup);
}

BENCHMARK(bmPromiseAwaitIntFromCustomAwaiter);

void bmPromiseAwaitYieldVoidInCoroutine(benchmark::State &state) {
  auto coroutine = []() -> Promise<void> { co_await yield(); };
  auto setup = [&coroutine, &state](const Loop &) -> Promise<void> {
    for (auto _ : state) {
      co_await coroutine();
    }
    co_return;
  };

  runMain<void>(setup);
}

BENCHMARK(bmPromiseAwaitYieldVoidInCoroutine);

void bmPromiseAwaitYieldIntInCoroutine(benchmark::State &state) {
  auto coroutine = []() -> Promise<void> { co_await yieldInt(); };
  auto setup = [&coroutine, &state](const Loop &) -> Promise<void> {
    for (auto _ : state) {
      co_await coroutine();
    }
    co_return;
  };

  runMain<void>(setup);
}

BENCHMARK(bmPromiseAwaitYieldIntInCoroutine);

void bmPromiseAwaitMultiYield(benchmark::State &state) {
  const size_t num = 4'000'000'000;
  auto setup = [&state](const Loop &) -> Promise<void> {
    MultiPromise<unsigned int> gen = yield(num);
    for (auto _ : state) {
      co_await gen;
    }
    co_return;
  };

  runMain<void>(setup);
}

BENCHMARK(bmPromiseAwaitMultiYield);

void bmPromiseAwaitMultiYieldNext(benchmark::State &state) {
  const size_t num = 4'000'000'000;
  auto setup = [&state](const Loop &) -> Promise<void> {
    MultiPromise<unsigned int> gen = yield(num);
    for (auto _ : state) {
      co_await gen.next();
    }
    co_return;
  };

  runMain<void>(setup);
}

BENCHMARK(bmPromiseAwaitMultiYieldNext);

} // namespace

BENCHMARK_MAIN();
