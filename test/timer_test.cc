
#include "promise.h"
#include "timer.h"

#include "test_util.h"

#include <gtest/gtest.h>

TEST(TimerTest, simpleWait) {
  bool ran = false;
  auto setup = [&](uv_loop_t *loop) -> uvco::Promise<void> {
    co_await uvco::wait(loop, 10);
    ran = true;
  };

  run_loop(setup);
  EXPECT_TRUE(ran);
}

TEST(TimerTest, tickerTest) {
  constexpr static uint64_t count = 3;
  uint64_t counter = 0;
  auto setup = [&](uv_loop_t *loop) -> uvco::Promise<void> {
    auto ticker = uvco::tick(loop, 10, count);
    uvco::MultiPromise<uint64_t> tickerProm = ticker->ticker();
    while (true) {
      std::optional<uint64_t> got = co_await tickerProm;
      if (got) {
        EXPECT_EQ(*got, counter);
        ++counter;
      } else {
        break;
      }
    }
  };

  run_loop(setup);
  EXPECT_EQ(counter, count);
}

TEST(TimerTest, infiniteTickerTest) {
  constexpr static uint64_t count = 3;
  uint64_t counter = 0;
  auto setup = [&](uv_loop_t *loop) -> uvco::Promise<void> {
    auto ticker = uvco::tick(loop, 10, 0);
    uvco::MultiPromise<uint64_t> tickerProm = ticker->ticker();
    for (counter = 0; counter < count; ++counter) {
      EXPECT_EQ(counter, *(co_await tickerProm));
    }
    co_await ticker->close();
  };

  run_loop(setup);
  EXPECT_EQ(counter, count);
}

TEST(TimerTest, finiteTickerTest) {
  constexpr static uint64_t count = 30;
  constexpr static uint64_t stopAfter = 3;
  uint64_t counter = 0;
  auto setup = [&](uv_loop_t *loop) -> uvco::Promise<void> {
    auto ticker = uvco::tick(loop, 10, count);
    uvco::MultiPromise<uint64_t> tickerProm = ticker->ticker();
    for (counter = 0; counter < stopAfter; ++counter) {
      EXPECT_EQ(counter, *(co_await tickerProm));
    }
    co_await ticker->close();
  };

  run_loop(setup);
  EXPECT_EQ(counter, stopAfter);
}
