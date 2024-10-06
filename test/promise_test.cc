#include <fmt/core.h>
#include <gtest/gtest.h>

#include "test_util.h"
#include "uvco/exception.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"
#include "uvco/timer.h"

#include <string_view>
#include <utility>

using namespace uvco;

namespace {

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
    Promise<int> promise = []() -> uvco::Promise<int> {
      co_await yield();
      co_return 1;
    }();
    EXPECT_EQ(co_await promise, 1);
    EXPECT_THROW({ co_await promise; }, UvcoException);
  };

  run_loop(setup);
}

TEST(PromiseTest, stackVariableAccessedSafely) {
  // Test fails through asan if stackVar is accessed in a use-after-return
  // manner.
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    unsigned int stackVar = 0;
    auto p = [](unsigned int &stackVar) -> uvco::Promise<void> {
      co_await yield();
      stackVar = 1;
      co_await yield();
      co_return;
    }(stackVar);

    co_await p;
  };

  run_loop(setup);
}

TEST(PromiseTest, awaitTwiceImmediateReturn) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    Promise<int> promise = []() -> uvco::Promise<int> { co_return 1; }();
    EXPECT_EQ(co_await promise, 1);
    EXPECT_THROW({ co_await promise; }, UvcoException);
  };

  run_loop(setup);
}

TEST(PromiseTest, yield) {
  bool awaited = false;
  auto setup = [&](const Loop &loop) -> uvco::Promise<void> {
    co_await yield();
    awaited = true;
    co_return;
  };

  run_loop(setup);
  EXPECT_TRUE(awaited);
}

// Only run this test with --gtest_filter=PromiseTest.yieldBench in Release
// builds
//
// Tests how efficient the event loop is at suspending/resuming a coroutine.
//
// Intel Core i5-7300U @ 2.6 GHz: 15 million iterations per second / 70 ns per
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
// Intel Core i5-7300U @ 2.6 GHz: 8.3 million iterations per second / 120 ns per
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

// This appears especially efficient; back-and-forth yielding occurs
// at only 32 ns overhead (that's 31 million iterations per second).
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
  co_await yield();
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

#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
TEST(PromiseTest, danglingReferenceCrashesAsan) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    Promise<void> promise =
        testTemporaryFunction(loop, fmt::format("Hello {}", "World"));

    // This will crash in asan.
    co_await promise;
  };

  EXPECT_DEATH({ run_loop(setup); }, "stack-use-after-scope");
}
#endif

TEST(PromiseTest, movePromiseBetweenFunctions) {
  auto coro1 = [](Promise<int> promise) -> uvco::Promise<void> {
    EXPECT_EQ(co_await promise, 42);
  };
  auto coro2 = [&](Promise<int> promise) -> uvco::Promise<void> {
    co_return co_await coro1(std::move(promise));
  };

  auto setup = [&](const Loop &loop) -> uvco::Promise<void> {
    Promise<int> promise1 = []() -> uvco::Promise<int> {
      co_await yield();
      co_return 42;
    }();

    co_await coro2(std::move(promise1));
  };

  run_loop(setup);
}

TEST(PromiseTest, destroyWithoutResume) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    Promise<int> promise = []() -> uvco::Promise<int> {
      co_await yield();
      co_return 1;
    }();
    co_return;
  };

  run_loop(setup);
}

TEST(PromiseTest, cancellation) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    auto awaited = [&loop]() -> uvco::Promise<int> {
      co_await sleep(loop, 1);
      co_return 3;
    };

    Promise<int> promise = awaited();
    PromiseHandle<int> handle = promise.handle();

    auto awaiter = [](Promise<int> promise) -> uvco::Promise<void> {
      EXPECT_THROW({ co_await promise; }, UvcoException);
    };

    Promise<void> awaiterPromise = awaiter(std::move(promise));
    handle.cancel();
    co_await awaiterPromise;
    co_return;
  };

  run_loop(setup);
}

TEST(PromiseTest, voidCancellation) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    auto awaited = []() -> uvco::Promise<void> { co_await yield(); };

    Promise<void> promise = awaited();
    PromiseHandle<void> handle = promise.handle();

    auto awaiter = [](Promise<void> cancelVictim,
                      int /*bogus*/) -> uvco::Promise<void> {
      EXPECT_THROW({ co_await cancelVictim; }, UvcoException);
    };

    handle.cancel();
    Promise<void> awaiterPromise = awaiter(std::move(promise), 1);
    co_await awaiterPromise;
    co_return;
  };

  run_loop(setup);
}

TEST(PromiseTest, voidEarlyCancellation) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    auto awaited = []() -> uvco::Promise<void> { co_await yield(); };

    Promise<void> promise = awaited();
    PromiseHandle<void> handle = promise.handle();
    handle.cancel();

    auto awaiter = [](Promise<void> cancelVictim,
                      int /*bogus*/) -> uvco::Promise<void> {
      EXPECT_THROW({ co_await cancelVictim; }, UvcoException);
    };

    Promise<void> awaiterPromise = awaiter(std::move(promise), 1);
    handle.cancel();
    co_await awaiterPromise;
    co_return;
  };

  run_loop(setup);
}

TEST(PromiseTest, voidFromCoroutineCancellation) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    auto awaited = []() -> uvco::Promise<void> { co_await yield(); };

    Promise<void> promise = awaited();
    PromiseHandle<void> handle = promise.handle();

    auto awaiter = [&handle](Promise<void> cancelVictim,
                             int /*bogus*/) -> uvco::Promise<void> {
      handle.cancel();
      EXPECT_THROW({ co_await cancelVictim; }, UvcoException);
    };

    Promise<void> awaiterPromise = awaiter(std::move(promise), 1);
    handle.cancel();
    co_await awaiterPromise;
    co_return;
  };

  run_loop(setup);
}

TEST(PromiseTest, noImplicitPromiseObjectInitialization) {
  // This is a test derived from the voidCancellation case. It fails
  // spectacularly if a promise object is instantiated from function arguments.
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    auto awaited = []() -> uvco::Promise<void> { co_await yield(); };

    Promise<void> promise = awaited();
    PromiseHandle<void> handle = promise.handle();

    // Problem: if we have a single promise argument, the compiler will just
    // construct the coroutine's promise object from it.
    auto awaiter = [](Promise<void> cancelVictim) -> uvco::Promise<void> {
      EXPECT_THROW({ co_await cancelVictim; }, UvcoException);
    };

    Promise<void> awaiterPromise = awaiter(std::move(promise));
    handle.cancel();
    co_await awaiterPromise;
    co_return;
  };

  run_loop(setup);
}

} // namespace
