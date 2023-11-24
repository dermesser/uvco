
#include "channel.h"
#include "internal_utils.h"
#include "test_util.h"

#include <gtest/gtest.h>

namespace {
using namespace uvco;
}

TEST(BQTest, basicPushPop) {
  BoundedQueue<int> bq{4};

  EXPECT_EQ(bq.size(), 0);
  EXPECT_TRUE(bq.empty());

  bq.push(1);
  bq.push(2);

  EXPECT_EQ(bq.size(), 2);
  EXPECT_EQ(bq.pop(), 1);

  bq.push(3);
  bq.push(4);

  EXPECT_EQ(bq.size(), 3);
  EXPECT_EQ(bq.pop(), 2);

  bq.push(5);
  bq.push(6);

  EXPECT_EQ(bq.size(), 4);
  EXPECT_EQ(bq.pop(), 3);

  bq.push(7);

  EXPECT_EQ(bq.size(), 4);
  EXPECT_EQ(bq.pop(), 4);
}

TEST(BQTest, pushTooMany) {
  BoundedQueue<int> bq(2);
  bq.push(1);
  bq.push(2);
  EXPECT_DEATH({ bq.push(3); }, "hasSpace");
}

TEST(BQTest, popEmpty) {
  BoundedQueue<int> bq(2);
  bq.push(1);
  bq.push(2);
  EXPECT_EQ(bq.pop(), 1);
  EXPECT_EQ(bq.pop(), 2);
  EXPECT_EQ(bq.size(), 0);
  EXPECT_DEATH({ bq.pop(); }, "size\\(\\) > 0");
}

TEST(ChannelTest, basicWriteRead) {

  auto setup = [&](uv_loop_t *) -> Promise<void> {
    Channel<int> ch{3};

    co_await ch.put(1);
    co_await ch.put(2);
    EXPECT_EQ(co_await ch.get(), 1);
  };

  run_loop(setup);
}

TEST(ChannelTest, blockingRead) {

  auto drain = [](Channel<int> &ch) -> Promise<void> {
    for (int i = 1; i < 3; ++i) {
      EXPECT_EQ(co_await ch.get(), i);
    }
  };
  auto setup = [&](uv_loop_t *) -> Promise<void> {
    Channel<int> ch{3};

    Promise<void> drainer = drain(ch);

    co_await ch.put(1);
    co_await ch.put(2);
    co_await ch.put(3);
    co_await ch.put(4);

    EXPECT_EQ(co_await ch.get(), 3);
    EXPECT_EQ(co_await ch.get(), 4);
  };

  run_loop(setup);
}

TEST(ChannelTest, blockingWriteBench) {

  auto source = [](Channel<int> &ch, int n) -> Promise<void> {
    for (int i = 1; i < n + 1; ++i) {
      co_await ch.put(i);
    }
  };
  auto setup = [&](uv_loop_t *) -> Promise<void> {
    Channel<int> ch{2};
    constexpr static int N = 1000;

    Promise<void> sourcer = source(ch, N);

    for (int i = 1; i < N; ++i) {
      EXPECT_EQ(co_await ch.get(), i);
    }
  };

  run_loop(setup);
}

TEST(ChannelTest, multipleWaiters) {

  auto reader = [](Channel<int> &ch) -> Promise<void> { co_await ch.get(); };

  auto setup = [&](uv_loop_t *) -> Promise<void> {
    Channel<int> ch{2};

    Promise<void> p1 = reader(ch);
    EXPECT_THROW({ Promise<void> p2 = reader(ch); }, UvcoException);
    co_await ch.put(1);
    co_await ch.put(2);
  };

  run_loop(setup);
}
