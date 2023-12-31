
#include "channel.h"
#include "test_util.h"

#include <gtest/gtest.h>

namespace {
using namespace uvco;
}

TEST(BQTest, basicPushPop) {
  BoundedQueue<int> bque{4};

  EXPECT_EQ(bque.size(), 0);
  EXPECT_TRUE(bque.empty());

  bque.put(1);
  bque.put(2);

  EXPECT_EQ(bque.size(), 2);
  EXPECT_EQ(bque.get(), 1);

  bque.put(3);
  bque.put(4);

  EXPECT_EQ(bque.size(), 3);
  EXPECT_EQ(bque.get(), 2);

  bque.put(5);
  bque.put(6);

  EXPECT_EQ(bque.size(), 4);
  EXPECT_EQ(bque.get(), 3);

  bque.put(7);

  EXPECT_EQ(bque.size(), 4);
  EXPECT_EQ(bque.get(), 4);
}

TEST(BQTest, pushTooMany) {
  BoundedQueue<int> bq(2);
  bq.put(1);
  bq.put(2);
  EXPECT_DEATH({ bq.put(3); }, "hasSpace");
}

TEST(BQTest, popEmpty) {
  BoundedQueue<int> bq(2);
  bq.put(1);
  bq.put(2);
  EXPECT_EQ(bq.get(), 1);
  EXPECT_EQ(bq.get(), 2);
  EXPECT_EQ(bq.size(), 0);
  EXPECT_DEATH({ bq.get(); }, "!empty");
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
    Channel<int> chan{3};

    Promise<void> drainer = drain(chan);

    Promise<void> put1 = chan.put(1);
    Promise<void> put1b, put1c;
    // Test copy and move assignment.
    put1b = put1;
    put1c = std::move(put1b);

    co_await put1c;
    co_await chan.put(2);
    co_await chan.put(3);
    co_await chan.put(4);

    EXPECT_EQ(co_await chan.get(), 3);
    EXPECT_EQ(co_await chan.get(), 4);
    co_await drainer;
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
    Channel<int> chan{2};
    constexpr static int N = 1000;

    Promise<void> sourcer = source(chan, N);

    for (int i = 1; i < N; ++i) {
      EXPECT_EQ(co_await chan.get(), i);
    }
    co_await sourcer;
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
    co_await p1;
  };

  run_loop(setup);
}
