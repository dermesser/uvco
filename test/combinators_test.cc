
#include "uvco/close.h"
#include "uvco/combinators.h"
#include "uvco/exception.h"
#include "uvco/pipe.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"
#include "uvco/stream.h"
#include "uvco/timer.h"

#include "test_util.h"

#include <cstddef>
#include <exception>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <uv.h>
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

TEST(CombinatorsTest, waitAnyDoubleResult) {
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

TEST(CombinatorsTest, waitAnyThrows) {
  const auto main = [&](const Loop &) -> Promise<void> {
    const auto oneYield = [&] -> Promise<std::string_view> {
      co_return "hello";
    };
    const auto throws = [&] -> Promise<int> {
      co_await yield();
      throw std::runtime_error("test error");
      co_return 123;
    };

    Promise<std::string_view> p1{oneYield()};
    Promise<int> p2{throws()};

    const std::vector<std::variant<std::string_view, int>> result1 =
        co_await waitAny(p1, p2);
    BOOST_ASSERT(result1.size() == 1);
    BOOST_ASSERT(result1[0].index() == 0);
    EXPECT_EQ("hello", std::get<0>(result1[0]));

    EXPECT_THROW({ co_await waitAny(p1, p2); }, std::runtime_error);

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

TEST(CombinatorsTest, raceThrowsIgnored) {
  const auto main = [&](const Loop &loop) -> Promise<void> {
    const auto oneYield = [&] -> Promise<std::string_view> {
      co_await yield();
      co_return "hello";
    };
    const auto throws = [&] -> Promise<int> {
      // We never sleep this long, the coroutine is just cancelled.
      co_await sleep(loop, 1000);
      throw std::runtime_error("test error");
      co_return 123;
    };

    EXPECT_EQ(1, (co_await race(oneYield(), throws())).size());

    co_return;
  };

  run_loop(main);
}

TEST(CombinatorsTest, raceThrows) {
  const auto main = [&](const Loop &loop) -> Promise<void> {
    const auto oneYield = [&] -> Promise<std::string_view> {
      co_await sleep(loop, 1000);
      co_return "hello";
    };
    const auto throws = [&] -> Promise<int> {
      // We never sleep this long, the coroutine is just cancelled.
      co_await yield();
      throw std::runtime_error("test error");
    };

    EXPECT_THROW({ co_await race(oneYield(), throws()); }, std::runtime_error);

    co_return;
  };

  run_loop(main);
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

TEST(CombinatorsTest, waitAllThrows) {
  const auto main = [&](const Loop &) -> Promise<void> {
    const auto throws = [&] -> Promise<int> {
      co_await yield();
      throw std::runtime_error("test error");
      co_return 123;
    };

    auto promise = waitAll(oneYield(), throws());
    EXPECT_THROW({ co_await promise; }, std::runtime_error);

    co_return;
  };

  run_loop(main);
}

TEST(WaitPointTest, basic) {
  const auto main = [&](const Loop &) -> Promise<void> {
    WaitPoint waitPoint;

    // Empty WaitPoint can release (no-op)
    waitPoint.releaseOne();
    waitPoint.releaseAll();

    Promise<void> one = waitPoint.wait();
    Promise<void> two = waitPoint.wait();

    co_await yield();

    EXPECT_FALSE(one.ready());
    EXPECT_FALSE(two.ready());

    waitPoint.releaseAll();

    co_await yield();

    EXPECT_TRUE(one.ready());
    EXPECT_TRUE(two.ready());

    co_return;
  };

  run_loop(main);
}

TEST(WaitPointTest, releaseOne) {
  const auto main = [&](const Loop &) -> Promise<void> {
    WaitPoint waitPoint;

    Promise<void> one = waitPoint.wait();
    Promise<void> two = waitPoint.wait();

    co_await yield();

    EXPECT_FALSE(one.ready());
    EXPECT_FALSE(two.ready());

    waitPoint.releaseOne();

    co_await yield();

    EXPECT_TRUE(one.ready());
    EXPECT_FALSE(two.ready());

    waitPoint.releaseOne();

    co_await yield();

    EXPECT_TRUE(two.ready());

    co_return;
  };

  run_loop(main);
}

TEST(TaskSetTest, basic) {
  const auto main = [&](const Loop &loop) -> Promise<void> {
    auto taskSet = TaskSet::create();
    bool ran = false;

    EXPECT_TRUE(taskSet->empty());
    EXPECT_TRUE(taskSet->onEmpty().ready());

    {
      taskSet->add([&loop](bool *ran) -> Promise<void> {
        co_await sleep(loop, 1);
        *ran = true;
      }(&ran));
    }

    EXPECT_FALSE(ran);
    EXPECT_FALSE(taskSet->empty());
    co_await taskSet->onEmpty();
    EXPECT_TRUE(taskSet->empty());

    EXPECT_TRUE(ran);

    co_return;
  };

  run_loop(main);
}

struct WeirdException {};

TEST(TaskSetTest, throwsError) {
  const auto main = [&](const Loop &loop) -> Promise<void> {
    auto taskSet = TaskSet::create();
    unsigned int numRan{};

    { // Handle error without error callback having been set.
      taskSet->add([&loop](unsigned int *ran) -> Promise<void> {
        co_await sleep(loop, 1);
        *ran += 1;
        throw UvcoException(UV_EINVAL, "Test exception");
      }(&numRan));
    }

    { // Handle error without error callback having been set.
      taskSet->add([&loop](unsigned int *ran) -> Promise<void> {
        co_await sleep(loop, 1);
        *ran += 1;
        throw WeirdException{};
      }(&numRan));
    }

    EXPECT_FALSE(taskSet->empty());
    co_await taskSet->onEmpty();
    EXPECT_TRUE(taskSet->empty());

    EXPECT_EQ(2, numRan);

    co_return;
  };

  run_loop(main);
}

TEST(TaskSetTest, throwsErrorWithCallback) {
  const auto main = [&](const Loop &loop) -> Promise<void> {
    auto taskSet = TaskSet::create();
    unsigned int numRan{};
    unsigned int numExceptions{};

    auto onError = [&numExceptions](TaskSet::Id,
                                    const std::exception_ptr & /*eptr*/) {
      numExceptions += 1;
    };
    taskSet->setOnError(std::move(onError));

    {
      taskSet->add([&loop](unsigned int *ran) -> Promise<void> {
        co_await sleep(loop, 1);
        *ran += 1;
        throw UvcoException(UV_EINVAL, "Test exception");
      }(&numRan));
    }
    {
      taskSet->add([&loop](unsigned int *ran) -> Promise<void> {
        co_await sleep(loop, 1);
        *ran += 1;
        throw WeirdException{};
      }(&numRan));
    }

    EXPECT_FALSE(taskSet->empty());
    co_await taskSet->onEmpty();
    EXPECT_TRUE(taskSet->empty());

    EXPECT_EQ(2, numRan);
    EXPECT_EQ(2, numExceptions);

    co_return;
  };

  run_loop(main);
}
} // namespace
