
#include <fcntl.h>
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <gtest/gtest.h>
#include <uv.h>

#include "test_util.h"
#include "uvco/async_work.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/promise.h"
#include "uvco/timer.h"

#include <atomic>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace uvco;

TEST(AsyncWorkTest, basicVoidFunction) {
  bool ran = false;
  auto work = [&]() -> void { ran = true; };

  auto setup = [&](const Loop &loop) -> Promise<void> {
    co_await submitWork<void>(loop, work);
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
      promises.push_back(submitWork<void>(loop, [i, &work]() { work(i); }));
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

TEST(AsyncWorkTest, cancelled) {
  auto work = []() -> void {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  };

  auto setup = [&](const Loop &loop) -> Promise<void> {
    auto p = submitWork<void>(loop, work);
    co_return;
  };

  run_loop(setup);
}

TEST(AsyncWorkTest, cancelledLate) {
  std::atomic<bool> started;

  auto work = [&started]() -> void {
    started.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  };

  auto setup = [&](const Loop &loop) -> Promise<void> {
    auto p = submitWork<void>(loop, work);
    co_await sleep(loop, 1);
    co_return;
  };

  run_loop(setup);
  EXPECT_TRUE(started);
}

TEST(AsyncWorkTest, exceptionThrown) {
  auto work = []() -> void { throw std::runtime_error("test"); };

  auto setup = [&](const Loop &loop) -> Promise<void> {
    auto p = submitWork<void>(loop, work);
    try {
      co_await p;
      EXPECT_TRUE(false) << "Expected exception";
    } catch (const std::runtime_error &e) {
      EXPECT_STREQ(e.what(), "test");
    }
  };

  run_loop(setup);
}

TEST(AsyncWorkTest, exceptionThrownForValue) {
  auto work = []() -> unsigned { throw std::runtime_error("test"); };

  auto setup = [&](const Loop &loop) -> Promise<void> {
    auto p = submitWork<unsigned>(loop, work);
    try {
      co_await p;
      EXPECT_TRUE(false) << "Expected exception";
    } catch (const std::runtime_error &e) {
      EXPECT_STREQ(e.what(), "test");
    }
  };
  run_loop(setup);
}

TEST(ThreadLocalKeyTest, basicSingleThread) {
  ThreadLocalKey<unsigned> key;

  key.set(42);
  EXPECT_EQ(key.get(), 42);

  key.set(100);
  EXPECT_EQ(key.get(), 100);

  key.del();
  EXPECT_NO_THROW({
    key.set(7);
    EXPECT_EQ(key.get(), 7);
  });
}

TEST(ThreadLocalKeyTest, differentThreadsDifferentValues) {
  ThreadLocalKey<unsigned> key;

  auto setup = [&key](const Loop &loop) -> Promise<void> {
    key.set(1);
    EXPECT_EQ(key.get(), 1);

    co_await submitWork<void>(loop, [&key]() {
      key.set(2);
      EXPECT_EQ(key.get(), 2);
      key.del();
    });

    EXPECT_EQ(1, key.get());

    co_return;
  };

  run_loop(setup);
  key.del();
}

} // namespace
