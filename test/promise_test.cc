
#include "exception.h"
#include "loop/loop.h"
#include "promise/promise.h"
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
