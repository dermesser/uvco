
#include "exception.h"
#include "loop/loop.h"
#include "promise/multipromise.h"
#include "promise/promise.h"
#include "test_util.h"
#include "timer.h"

#include <coroutine>
#include <optional>
#include <string>

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <uv.h>

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
    co_await sleep(loop, 1);
  }
  throw UvcoException("ticker");
}

TEST(MultiPromiseTest, exception) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    MultiPromise<int> ticker = miniTicker(loop);
    EXPECT_EQ(co_await ticker, 0);
    EXPECT_EQ(co_await ticker, 1);
    EXPECT_EQ(co_await ticker, 2);
    EXPECT_THROW({ co_await ticker; }, UvcoException);
  };

  run_loop(setup);
}

} // namespace
