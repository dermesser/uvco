#include <gtest/gtest.h>
#include <sys/socket.h>
#include <uv.h>

#include "test_util.h"
#include "uvco/exception.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"

#include <coroutine>
#include <optional>
#include <string>

namespace {
using namespace uvco;

TEST(MultiPromiseTest, standardGenerator) {
  constexpr static int countMax = 10;
  auto generator = []() -> uvco::MultiPromise<int> {
    for (int i = 0; i < countMax; ++i) {
      co_yield i;
    }
  };

  auto setup = [&generator](const Loop &loop) -> uvco::Promise<void> {
    MultiPromise<int> ticker = generator();
    for (int i = 0; i < countMax; ++i) {
      const auto value = co_await ticker;
      EXPECT_TRUE(value.has_value());
      EXPECT_EQ(i, value.value());
    }
    EXPECT_EQ(co_await ticker, std::nullopt);
    EXPECT_EQ(co_await ticker, std::nullopt);
  };

  run_loop(setup);
}

TEST(MultiPromiseTest, generatorThrows) {
  constexpr static int countMax = 3;
  auto generator = []() -> uvco::MultiPromise<int> {
    for (int i = 0; i < countMax; ++i) {
      co_yield i;
    }
    throw UvcoException("ticker");
  };

  auto setup = [&generator](const Loop &loop) -> uvco::Promise<void> {
    MultiPromise<int> ticker = generator();
    for (int i = 0; i < countMax; ++i) {
      const auto value = co_await ticker;
      EXPECT_TRUE(value.has_value());
      EXPECT_EQ(i, value.value());
    }
    // Repeatedly throws.
    EXPECT_THROW({ co_await ticker; }, UvcoException);
    EXPECT_THROW({ co_await ticker; }, UvcoException);
  };

  run_loop(setup);
}

MultiPromise<int> miniTicker(const Loop &loop) {
  for (int i = 0; i < 3; ++i) {
    co_yield i;
    co_await yield();
  }
  throw UvcoException("ticker");
}

TEST(MultiPromiseTest, exceptionWithTimer) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    MultiPromise<int> ticker = miniTicker(loop);
    EXPECT_EQ(co_await ticker, 0);
    EXPECT_EQ(co_await ticker, 1);
    EXPECT_EQ(co_await ticker, 2);
    EXPECT_THROW({ co_await ticker; }, UvcoException);
  };

  run_loop(setup);
}

TEST(MultiPromiseTest, yield) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    static constexpr unsigned count = 10;
    MultiPromise<unsigned> ticker = yield(count);
    for (unsigned i = 0; i < count; ++i) {
      EXPECT_EQ(co_await ticker, i);
    }
  };

  run_loop(setup);
}

TEST(MultiPromiseTest, nextValue) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    MultiPromise<int> gen = []() -> uvco::MultiPromise<int> {
      co_yield 1;
      co_yield 2;
      co_yield 3;
    }();

    EXPECT_EQ(co_await gen.next(), std::optional{1});
    EXPECT_EQ(co_await gen.next(), std::optional{2});
    EXPECT_EQ(co_await gen.next(), std::optional{3});
    EXPECT_EQ(co_await gen.next(), std::nullopt);
  };

  run_loop(setup);
}

} // namespace
