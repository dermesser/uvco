
#include <atomic>
#include <functional>
#include <uv.h>

#include "test_util.h"

#include "uvco/async_work.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/promise.h"

#include <fcntl.h>
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <gtest/gtest.h>
#include <vector>

namespace {

using namespace uvco;

TEST(AsyncWorkTest, basicVoidFunction) {
  bool ran = false;
  auto work = [&]() -> void { ran = true; };

  auto setup = [&](const Loop &loop) -> Promise<void> {
    co_await submitWork(loop, work);
  };

  run_loop(setup);
  EXPECT_TRUE(ran);
}

TEST(AsyncWorkTest, scheduleMany) {
  static constexpr unsigned num = 100;
  std::atomic<unsigned> counter = 0;
  std::vector<unsigned> order;
  order.resize(num);
  auto work = [&](unsigned) -> void { ++counter; };

  auto setup = [&](const Loop &loop) -> Promise<void> {
    std::vector<Promise<void>> promises;
    for (unsigned i = 0; i < num; ++i) {
      promises.push_back(submitWork(loop, [i, &work]() { work(i); }));
    }

    unsigned waited{};
    for (auto &p : promises) {
      co_await p;
      ++waited;
    }
    EXPECT_EQ(waited, num);
  };

  run_loop(setup);
  EXPECT_EQ(counter, num);
}

TEST(AsyncWorkTest, resultReturned) {
  static constexpr unsigned num = 100;

  auto work = [](unsigned i) -> unsigned { return i; };

  auto setup = [&](const Loop &loop) -> Promise<void> {
    std::vector<Promise<unsigned>> promises;
    for (unsigned i = 0; i < num; ++i) {
      promises.push_back(
          submitWork(loop, std::function<unsigned()>{
                               [i, &work]() -> unsigned { return work(i); }}));
    }

    unsigned waited{};
    for (auto &p : promises) {
      auto result = co_await p;
      EXPECT_EQ(result, waited);
      ++waited;
    }
    EXPECT_EQ(waited, num);
  };

  run_loop(setup);
}

} // namespace
