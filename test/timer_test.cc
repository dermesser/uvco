
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
      if (co_await tickerProm) {
        ++counter;
      } else {
        break;
      }
    }
  };

  run_loop(setup);
  EXPECT_EQ(counter, count);
}
