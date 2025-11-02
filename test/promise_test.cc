#include <fmt/core.h>
#include <gtest/gtest.h>

#include "test_util.h"
#include "uvco/combinators.h"
#include "uvco/exception.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"

#include <string_view>
#include <utility>

using namespace uvco;

namespace {

TEST(PromiseTest, moveCtor) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    Promise<int> promise1 = []() -> Promise<int> { co_return 1; }();
    Promise<int> promise2 = std::move(promise1);
    EXPECT_EQ(co_await promise2, 1);
  };

  run_loop(setup);
}

TEST(PromiseTest, moveCtor2) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    Promise<int> promise1 = []() -> Promise<int> { co_return 1; }();
    Promise<int> promise2 = std::move(promise1);
    promise1 = std::move(promise2);
    EXPECT_EQ(co_await promise1, 1);
  };

  run_loop(setup);
}

TEST(PromiseTest, moveCtorVoid) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    Promise<void> promise1 = []() -> Promise<void> { co_return; }();
    Promise<void> promise2 = std::move(promise1);
    promise1 = std::move(promise2);
    co_await promise1;
  };

  run_loop(setup);
}

TEST(PromiseTest, moveCtorVoid2) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    Promise<void> promise1 = []() -> Promise<void> { co_return; }();
    Promise<void> promise2 = std::move(promise1);
    promise1 = []() -> Promise<void> {
      co_await yield();
      co_return;
    }();
    promise1 = std::move(promise2);
    co_await promise1;
  };

  run_loop(setup);
}

TEST(PromiseTest, awaitTwice) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    Promise<int> promise = []() -> Promise<int> {
      co_await yield();
      co_return 1;
    }();
    EXPECT_EQ(co_await promise, 1);
    EXPECT_THROW({ co_await promise; }, UvcoException);
  };

  run_loop(setup);
}

TEST(PromiseTest, awaitTwiceImmediateReturn) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    Promise<int> promise = []() -> Promise<int> { co_return 1; }();
    EXPECT_EQ(co_await promise, 1);
    EXPECT_THROW({ co_await promise; }, UvcoException);
  };

  run_loop(setup);
}

// Only run this test with --gtest_filter=PromiseTest.yieldBench in Release
// builds
//
// Tests how efficient the event loop is at suspending/resuming a coroutine.
//
// Intel Core i5-7300U @ 2.6 GHz: 15 million iterations per second / 70 ns per
// iteration
TEST(PromiseTest, DISABLED_yieldBench) {
  static constexpr unsigned iterations = 1'000'000;
  auto setup = [](const Loop &loop) -> Promise<void> {
    for (unsigned i = 0; i < iterations; ++i) {
      co_await yield();
    }
    co_return;
  };

  run_loop(setup);
}

// yieldIntBench: everything inline - 31% faster

struct YieldAwaiter_ {
  [[nodiscard]] static bool await_ready() noexcept { return false; }
  bool await_suspend(std::coroutine_handle<> handle) noexcept {
    Loop::enqueue(handle);
    return true;
  }
  [[nodiscard]] int await_resume() const noexcept { return 1; }
};

Promise<int> yieldInt() { co_return (co_await YieldAwaiter_{}); }

TEST(PromiseTest, DISABLED_yieldIntBench) {
  static constexpr unsigned iterations = 1'000'000;
  auto setup = [](const Loop &loop) -> Promise<void> {
    for (unsigned i = 0; i < iterations; ++i) {
      co_await yieldInt();
    }
    co_return;
  };

  run_loop(setup);
}

TEST(PromiseTest, yield) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    co_await yield();
    co_return;
  };

  run_loop(setup);
}

TEST(PromiseTest, yieldInt) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    BOOST_VERIFY(1 == co_await yieldInt());
    co_return;
  };

  run_loop(setup);
}

// Same as above, but with a separate coroutine, i.e. two levels of awaiting.
// Intel Core i5-7300U @ 2.6 GHz: 8.3 million iterations per second / 120 ns per
// iteration
TEST(PromiseTest, DISABLED_yieldCallBench) {
  static constexpr unsigned iterations = 1'000'000;
  auto coroutine = [](const Loop &loop) -> Promise<void> { co_await yield(); };
  auto setup = [&](const Loop &loop) -> Promise<void> {
    for (unsigned i = 0; i < iterations; ++i) {
      co_await coroutine(loop);
    }
    co_return;
  };

  run_loop(setup);
}

// This appears especially efficient; back-and-forth yielding occurs
// at only 32 ns overhead (that's 31 million iterations per second).
TEST(PromiseTest, DISABLED_multiYieldBench) {
  static constexpr unsigned iterations = 1'000'000;
  auto setup = [](const Loop &loop) -> Promise<void> {
    MultiPromise<unsigned> multi = yield(iterations);
    for (unsigned i = 0; i < iterations; ++i) {
      co_await multi;
    }
    co_return;
  };

  run_loop(setup);
}

Promise<void> testTemporaryFunction(const Loop &loop,
                                    std::string_view message) {
  co_await yield();
  co_return;
}

TEST(PromiseTest, temporaryOk) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    // Temporary promise.
    co_await testTemporaryFunction(loop, fmt::format("Hello {}", "World"));
  };
  run_loop(setup);
}

TEST(PromiseTest, movePromiseBetweenFunctions) {
  auto coro1 = [](Promise<int> promise) -> Promise<void> {
    EXPECT_EQ(co_await promise, 42);
  };
  auto coro2 = [&](Promise<int> promise) -> Promise<void> {
    co_return co_await coro1(std::move(promise));
  };

  auto setup = [&](const Loop &loop) -> Promise<void> {
    Promise<int> promise1 = []() -> Promise<int> {
      co_await yield();
      co_return 42;
    }();

    co_await coro2(std::move(promise1));
  };

  run_loop(setup);
}

TEST(PromiseTest, destroyWithoutResume) {
  bool initialized = false;
  bool ran = false;
  auto setup = [&](const Loop &loop) -> Promise<void> {
    Promise<int> promise = [&]() -> Promise<int> {
      initialized = true;
      co_await yield();
      ran = true;
      co_return 1;
    }();

    co_return;
  };

  run_loop(setup);
  EXPECT_TRUE(initialized);
  EXPECT_FALSE(ran);
}

TEST(PromiseTest, destroyVoidWithoutResume) {
  bool initialized = false;
  bool ran = false;
  auto setup = [&](const Loop &loop) -> Promise<void> {
    Promise<void> promise = [&]() -> Promise<void> {
      initialized = true;
      co_await yield();
      ran = true;
    }();

    co_return;
  };

  run_loop(setup);
  EXPECT_TRUE(initialized);
  EXPECT_FALSE(ran);
}

} // namespace
