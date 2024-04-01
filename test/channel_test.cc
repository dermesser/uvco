
#include "channel.h"
#include "run.h"
#include "test_util.h"
#include "timer.h"

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
  BoundedQueue<int> bque(2);
  bque.put(1);
  bque.put(2);
  // Only dies in CMAKE_BUILD_TYPE=Debug.
  EXPECT_THROW({ bque.put(3); }, UvcoException);
}

TEST(BQTest, popEmpty) {
  BoundedQueue<int> bque(2);
  bque.put(1);
  bque.put(2);
  EXPECT_EQ(bque.get(), 1);
  EXPECT_EQ(bque.get(), 2);
  EXPECT_EQ(bque.size(), 0);
  // Only dies in CMAKE_BUILD_TYPE=Debug.
  EXPECT_THROW({ bque.get(); }, UvcoException);
}

TEST(ChannelTest, basicWriteRead) {
  auto setup = [&](const Loop &) -> Promise<void> {
    Channel<int> chan{3};

    co_await chan.put(1);
    co_await chan.put(2);
    EXPECT_EQ(co_await chan.get(), 1);
  };

  run_loop(setup);
}

TEST(ChannelTest, blockingRead) {

  auto drain = [](Channel<int> &chan) -> Promise<void> {
    for (int i = 1; i < 3; ++i) {
      EXPECT_EQ(co_await chan.get(), i);
    }
  };

  bool reachedEnd = false;
  auto setup = [&](const Loop &) -> Promise<void> {
    Channel<int> chan{3};

    Promise<void> drainer = drain(chan);

    Promise<void> put1 = chan.put(1);
    Promise<void> put1b;
    Promise<void> put1c;
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
    reachedEnd = true;
  };

  run_loop(setup);
  // May not be the case if a co_await stalled: then run_loop finishes
  // prematurely.
  EXPECT_TRUE(reachedEnd);
}

TEST(ChannelTest, blockingWriteBench) {

  auto source = [](Channel<int> &chan, int n_iter) -> Promise<void> {
    for (int i = 1; i < n_iter + 1; ++i) {
      co_await chan.put(i);
    }
  };

  bool reachedEnd = false;
  auto setup = [&](const Loop &) -> Promise<void> {
    Channel<int> chan{2};
    constexpr static int N_iter = 10;

    Promise<void> sourcer = source(chan, N_iter);

    for (int i = 1; i < N_iter; ++i) {
      EXPECT_EQ(co_await chan.get(), i);
    }
    co_await sourcer;
    reachedEnd = true;
  };

  run_loop(setup);
  // May not be the case if a co_await stalled: then run_loop finishes
  // prematurely.
  EXPECT_TRUE(reachedEnd);
}

TEST(ChannelTest, multipleWaiters) {

  auto reader = [](Channel<int> &chan) -> Promise<void> {
    co_await chan.get();
  };

  bool reachedEnd = false;
  auto setup = [&](const Loop &) -> Promise<void> {
    Channel<int> chan{2};

    Promise<void> prom1 = reader(chan);
    Promise<void> prom2 = reader(chan);
    co_await chan.put(1);
    co_await chan.put(2);
    co_await prom1;
    co_await prom2;
    reachedEnd = true;
  };

  run_loop(setup);
  // May not be the case if a co_await stalled: then run_loop finishes
  // prematurely.
  EXPECT_TRUE(reachedEnd);
}

TEST(ChannelTest, tooManyWaiters) {
  auto reader = [](Channel<int> &chan) -> Promise<void> {
    try {
      co_await chan.get();
    } catch (const UvcoException &e) {
      fmt::print(stderr, "Caught exception: {}\n", e.what());
      throw e;
    }
  };

  bool reachedEnd = false;
  auto setup = [&](const Loop &) -> Promise<void> {
    Channel<int> chan{2, 1};

    Promise<void> prom1 = reader(chan);
    Promise<void> prom2 = reader(chan);
    co_await chan.put(1);
    co_await prom1;
    EXPECT_THROW({ co_await prom2; }, UvcoException);
    reachedEnd = true;
  };

  run_loop(setup);
  // May not be the case if a co_await stalled: then run_loop finishes
  // prematurely.
  EXPECT_TRUE(reachedEnd);
}
