
#include "uvco/combinators.h"
#include "uvco/exception.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"
#include "uvco/timer.h"

#include "test_util.h"

#include <cstddef>
#include <gtest/gtest.h>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace {
using namespace uvco;

TEST(CombinatorsTest, waitAndDrop) {

  size_t finishedNormally = 0;

  const auto main = [&](const Loop &) -> Promise<void> {
    const auto oneYield = [&] -> Promise<void> {
      co_await yield();
      finishedNormally++;
      co_return;
    };
    const auto twoYields = [&] -> Promise<void> {
      co_await yield();
      co_await yield();
      finishedNormally++;
      co_return;
    };

    co_await raceIgnore(oneYield(), twoYields());
    EXPECT_EQ(1, finishedNormally);
    co_return;
  };

  run_loop(main);
}

TEST(CombinatorsTest, waitEitherDoubleResult) {
  const auto main = [&](const Loop &) -> Promise<void> {
    const auto oneYield = [&] -> Promise<std::string_view> {
      co_await yield();
      co_return "hello";
    };
    const auto twoYields = [&] -> Promise<int> {
      co_await yield();
      co_await yield();
      co_return 123;
    };

    Promise<std::string_view> p1{oneYield()};
    Promise<int> p2{twoYields()};

    const std::vector<std::variant<std::string_view, int>> result1 =
        co_await waitAny(p1, p2);
    BOOST_ASSERT(result1.size() == 1);
    BOOST_ASSERT(result1[0].index() == 0);
    EXPECT_EQ("hello", std::get<0>(result1[0]));

    const std::vector<std::variant<std::string_view, int>> result2 =
        co_await waitAny(p1, p2);
    BOOST_ASSERT(result2.size() == 1);
    BOOST_ASSERT(result2[0].index() == 1);
    EXPECT_EQ(123, std::get<1>(result2[0]));

    co_return;
  };

  run_loop(main);
}

TEST(CombinatorsTest, race) {
  bool twoYieldsFinished = false;
  const auto main = [&](const Loop &loop) -> Promise<void> {
    const auto oneYield = [&] -> Promise<std::string_view> {
      co_await yield();
      co_return "hello";
    };
    const auto twoYields = [&] -> Promise<int> {
      // We never sleep this long, the coroutine is just cancelled.
      co_await sleep(loop, 1000);
      twoYieldsFinished = true;
      co_return 123;
    };

    Promise<std::string_view> p1{oneYield()};
    Promise<int> p2{twoYields()};

    const std::vector<std::variant<std::string_view, int>> result1 =
        co_await waitAny(p1, p2);
    BOOST_ASSERT(result1.size() == 1);
    BOOST_ASSERT(result1[0].index() == 0);
    EXPECT_EQ("hello", std::get<0>(result1[0]));

    co_return;
  };

  run_loop(main);
  EXPECT_FALSE(twoYieldsFinished);
}

TEST(CombinatorsTest, raceIgnore) {
  bool twoYieldsFinished = false;
  const auto main = [&](const Loop &loop) -> Promise<void> {
    const auto oneYield = [&] -> Promise<void> {
      co_await yield();
      co_return;
    };
    const auto twoYields = [&] -> Promise<void> {
      // We never sleep this long, the coroutine is just cancelled.
      co_await sleep(loop, 1000);
      twoYieldsFinished = true;
      co_return;
    };

    co_await raceIgnore(oneYield(), twoYields());
    co_return;
  };

  run_loop(main);
  EXPECT_FALSE(twoYieldsFinished);
}

Promise<std::string_view> oneYield() {
  co_await yield();
  co_return "hello";
}

Promise<int> twoYields() {
  co_await yield();
  co_await yield();
  co_return 123;
}

TEST(CombinatorsTest, waitAll) {
  const auto main = [&](const Loop &) -> Promise<void> {
    const auto result = co_await waitAll(oneYield(), twoYields());
    EXPECT_EQ("hello", std::get<0>(result));
    EXPECT_EQ(123, std::get<1>(result));

    co_return;
  };

  run_loop(main);
}

TEST(CombinatorsTest, waitAllStored) {
  const auto main = [&](const Loop &) -> Promise<void> {
    const auto promise = waitAll(oneYield(), twoYields());
    const auto result = co_await promise;
    EXPECT_EQ(std::make_tuple("hello", 123), result);

    co_return;
  };

  run_loop(main);
}

TEST(CombinatorsTest, waitAllTwice) {
  const auto main = [&](const Loop &) -> Promise<void> {
    auto promise = waitAll(oneYield(), twoYields());
    const auto result = co_await promise;
    EXPECT_EQ(std::make_tuple("hello", 123), result);
    EXPECT_THROW({ co_await promise; }, UvcoException);

    co_return;
  };

  run_loop(main);
}

} // namespace
