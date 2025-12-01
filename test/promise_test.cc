#include <coroutine>
#include <fmt/core.h>
#include <fmt/format.h>
#include <gtest/gtest.h>

#include "test_util.h"
#include "uvco/combinators.h"
#include "uvco/exception.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace uvco;

namespace {

TEST(PromiseTest, moveCtor) {
  auto setup = [](const Loop &) -> Promise<void> {
    Promise<int> promise1 = []() -> Promise<int> { co_return 1; }();
    Promise<int> promise2 = std::move(promise1);
    EXPECT_EQ(co_await promise2, 1);
  };

  run_loop(setup);
}

TEST(PromiseTest, moveCtor2) {
  auto setup = [](const Loop &) -> Promise<void> {
    Promise<int> promise1 = []() -> Promise<int> { co_return 1; }();
    Promise<int> promise2 = std::move(promise1);
    promise1 = std::move(promise2);
    EXPECT_EQ(co_await promise1, 1);
  };

  run_loop(setup);
}

TEST(PromiseTest, moveCtorVoid) {
  auto setup = [](const Loop &) -> Promise<void> {
    Promise<void> promise1 = []() -> Promise<void> { co_return; }();
    Promise<void> promise2 = std::move(promise1);
    promise1 = std::move(promise2);
    co_await promise1;
  };

  run_loop(setup);
}

TEST(PromiseTest, moveCtorVoid2) {
  auto setup = [](const Loop &) -> Promise<void> {
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
  auto setup = [](const Loop &) -> Promise<void> {
    Promise<int> promise = []() -> Promise<int> {
      co_await yield();
      co_return 1;
    }();
    EXPECT_EQ(co_await promise, 1);
    EXPECT_THROW({ co_await promise; }, UvcoException);
  };

  run_loop(setup);
}

TEST(PromiseTest, awaitConst) {
  auto setup = [](const Loop &) -> Promise<void> {
    const Promise<int> promise = []() -> Promise<int> {
      co_await yield();
      co_return 1;
    }();
    EXPECT_EQ(co_await promise, 1);
  };

  run_loop(setup);
}

TEST(PromiseTest, assignmentEquivalent) {
  auto setup = [](const Loop &) -> Promise<void> {
    auto coroutine = []() -> Promise<int> {
      co_await yield();
      co_return 1;
    };

    // Verify that awaiting a called coroutine and a stored promise both works.
    EXPECT_EQ(co_await coroutine(), 1);

    auto promise = coroutine();
    EXPECT_EQ(co_await promise, 1);
  };

  run_loop(setup);
}

TEST(PromiseTest, awaitTwiceImmediateReturn) {
  auto setup = [](const Loop &) -> Promise<void> {
    Promise<int> promise = []() -> Promise<int> { co_return 1; }();
    EXPECT_EQ(co_await promise, 1);
    EXPECT_THROW({ co_await promise; }, UvcoException);
  };

  run_loop(setup);
}

struct MovableOnly {
  MovableOnly() = default;
  MovableOnly(MovableOnly &&other) noexcept = default;
  MovableOnly &operator=(MovableOnly &&other) noexcept = default;
  MovableOnly(const MovableOnly &) = delete;
  MovableOnly &operator=(const MovableOnly &) = delete;
};

TEST(PromiseTest, awaitReturnsMovableOnly) {
  auto setup = [](const Loop &) -> Promise<void> {
    Promise<MovableOnly> promise = []() -> Promise<MovableOnly> {
      co_await yield();
      co_return MovableOnly{};
    }();
    MovableOnly value = co_await promise;
    (void)value;
  };

  run_loop(setup);
}

TEST(PromiseTest, unwrapReturnsMovableOnly) {
  auto setup = [](const Loop &) -> Promise<void> {
    Promise<MovableOnly> promise = []() -> Promise<MovableOnly> {
      co_await yield();
      co_return MovableOnly{};
    }();
    co_await yield();
    MovableOnly value = promise.unwrap();
    (void)value;
  };

  run_loop(setup);
}

TEST(PromiseTest, unwrapRethrows) {
  auto setup = [](const Loop &) -> Promise<void> {
    Promise<int> promise = []() -> Promise<int> {
      co_await yield();
      throw UvcoException("test exception");
    }();
    try {
      co_await yield();
      EXPECT_TRUE(promise.ready());
      promise.unwrap();
    } catch (const UvcoException &e) {
      EXPECT_STREQ(e.what(), "test exception");
    }
    co_return;
  };

  run_loop(setup);
}

template <typename PromiseType>
struct RvalueCoroutineFixture : public ::testing::Test {};
using RvalueCoroutineTypes = ::testing::Types<void, int>;
TYPED_TEST_SUITE(RvalueCoroutineFixture, RvalueCoroutineTypes);

// Tests that temporary values passed to a coroutine don't wreak havoc.
TYPED_TEST(RvalueCoroutineFixture, rvalueCoroutine) {
  auto setup = [](const Loop &) -> Promise<void> {
    // An rvalue `stringArg` is forbidden by the Coroutine constructors.
    const auto coroutine = [](std::string stringArg) -> Promise<TypeParam> {
      co_await yield();
      [[maybe_unused]]
      const std::string result = fmt::format("{}\n", stringArg);
      if constexpr (std::is_same_v<TypeParam, void>) {
        co_return;
      } else {
        co_return 1;
      }
    };

    std::optional<Promise<TypeParam>> promise1;
    std::optional<Promise<TypeParam>> promise2;
    std::optional<Promise<TypeParam>> promise3;

    {
      std::string string{"abc"};
      promise1 = coroutine(fmt::format("abc"));
      promise2 = coroutine("abc");
      promise3 = coroutine(std::move(string));
    }

    co_await promise1.value();
    co_await promise2.value();
    co_await promise3.value();
  };

  run_loop(setup);
}

struct YieldAwaiter_ {
  [[nodiscard]] static bool await_ready() noexcept { return false; }
  bool await_suspend(std::coroutine_handle<> handle) noexcept {
    Loop::enqueue(handle);
    return true;
  }
  [[nodiscard]] int await_resume() const noexcept { return 1; }
};

Promise<int> yieldInt() { co_return (co_await YieldAwaiter_{}); }

TEST(PromiseTest, yield) {
  auto setup = [](const Loop &) -> Promise<void> {
    co_await yield();
    co_return;
  };

  run_loop(setup);
}

TEST(PromiseTest, yieldInt) {
  auto setup = [](const Loop &) -> Promise<void> {
    BOOST_VERIFY(1 == co_await yieldInt());
    co_return;
  };

  run_loop(setup);
}

Promise<void> testTemporaryFunction(std::string_view message) {
  co_await yield();

  // Ensure that all memory is accessible, checked by asan
  for (char c : message) {
    EXPECT_NE(c, '\0');
  }

  co_return;
}

TEST(PromiseTest, temporaryOk) {
  auto setup = [](const Loop &) -> Promise<void> {
    // Temporary promise.
    co_await testTemporaryFunction(fmt::format("Hello {}", "World"));
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

  auto setup = [&](const Loop &) -> Promise<void> {
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
  auto setup = [&](const Loop &) -> Promise<void> {
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
  auto setup = [&](const Loop &) -> Promise<void> {
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
