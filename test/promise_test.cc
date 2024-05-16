
#include "exception.h"
#include "loop/loop.h"
#include "promise/multipromise.h"
#include "promise/promise.h"
#include "run.h"
#include "test_util.h"
#include "timer.h"

#include <fmt/core.h>
#include <gtest/gtest.h>
#include <string_view>
#include <utility>

namespace {
using namespace uvco;

TEST(PromiseTest, moveCtor) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    Promise<int> promise1 = []() -> uvco::Promise<int> { co_return 1; }();
    Promise<int> promise2 = std::move(promise1);
    EXPECT_EQ(co_await promise2, 1);
  };

  run_loop(setup);
}

TEST(PromiseTest, awaitTwice) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    Promise<int> promise = []() -> uvco::Promise<int> { co_return 1; }();
    EXPECT_EQ(co_await promise, 1);

    EXPECT_THROW({ co_await promise; }, UvcoException);
  };

  run_loop(setup);
}

TEST(PromiseTest, yield) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    co_await yield();
    co_return;
  };

  run_loop(setup);
}

// Only run this test with --gtest_filter=PromiseTest.yieldBench in Release
// builds
//
// Tests how efficient the event loop is at suspending/resuming a coroutine.
//
// Intel Core i5-7300U @ 2.6 GHz: 4.4 million iterations per second / 230 ns per
// iteration
TEST(PromiseTest, DISABLED_yieldBench) {
  static constexpr unsigned iterations = 1000000;
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    for (unsigned i = 0; i < iterations; ++i) {
      co_await yield();
    }
    co_return;
  };

  run_loop(setup);
}

// Same as above, but with a separate coroutine, i.e. two levels of awaiting.
// Intel Core i5-7300U @ 2.6 GHz: 2.6 million iterations per second / 380 ns per
// iteration
TEST(PromiseTest, DISABLED_yieldCallBench) {
  static constexpr unsigned iterations = 1000000;
  auto coroutine = [](const Loop &loop) -> uvco::Promise<void> {
    co_await yield();
  };
  auto setup = [&](const Loop &loop) -> uvco::Promise<void> {
    for (unsigned i = 0; i < iterations; ++i) {
      co_await coroutine(loop);
    }
    co_return;
  };

  run_loop(setup);
}

TEST(PromiseTest, DISABLED_multiYieldBench) {
  static constexpr unsigned iterations = 1000000;
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
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
  co_await sleep(loop, 1);
  fmt::print("Message is {}\n", message);
  co_return;
}

TEST(PromiseTest, temporaryOk) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    // Temporary promise.
    co_await testTemporaryFunction(loop, fmt::format("Hello {}", "World"));
  };
  run_loop(setup);
}

TEST(PromiseTest, danglingReferenceCrashesAsan) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    Promise<void> promise =
        testTemporaryFunction(loop, fmt::format("Hello {}", "World"));

    // This will crash in asan.
    co_await promise;
  };

#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
  EXPECT_DEATH({ run_loop(setup); }, "stack-use-after-return");
#endif
}

TEST(PromiseTest, movePromiseBetweenFunctions) {
  auto coro1 = [](Promise<int> promise) -> uvco::Promise<void> {
    EXPECT_EQ(co_await promise, 42);
  };
  auto coro2 = [&](Promise<int> promise) -> uvco::Promise<void> {
    co_return co_await coro1(std::move(promise));
  };

  auto setup = [&](const Loop &loop) -> uvco::Promise<void> {
    Promise<int> promise1 = [&loop]() -> uvco::Promise<int> {
      co_await sleep(loop, 5);
      co_return 42;
    }();

    co_await coro2(std::move(promise1));
  };

  run_loop(setup);
}

TEST(PromiseTest, destroyWithoutResume) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    Promise<int> promise = [&loop]() -> uvco::Promise<int> {
      co_await sleep(loop, 5);
      // Will put promise core in state finished, all good.
      co_return 1;
    }();
    co_return;
  };

  run_loop(setup);
}

} // namespace
