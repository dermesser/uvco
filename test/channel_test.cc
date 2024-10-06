
#include "uvco/channel.h"
#include "uvco/exception.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"

#include "test_util.h"

#include <gtest/gtest.h>
#include <utility>

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

// On my out-dated Core i5-7300U @ 2.60GHz, this results in about 130 ns per
// item.
TEST(ChannelTest, DISABLED_basicWriteReadBench) {
  static constexpr int N_iter = 1000000;
  auto reader = [](Channel<int> &chan) -> Promise<void> {
    for (int i = 1; i < N_iter; ++i) {
      EXPECT_EQ(co_await chan.get(), i);
    }
  };
  auto writer = [](Channel<int> &chan) -> Promise<void> {
    for (int i = 1; i < N_iter; ++i) {
      co_await chan.put(i);
    }
  };

  auto setup = [&](const Loop &) -> Promise<void> {
    Channel<int> chan{2};

    Promise<void> readerCoroutine = reader(chan);
    Promise<void> writerCoroutine = writer(chan);

    co_await readerCoroutine;
    co_await writerCoroutine;
  };

  run_loop(setup);
}

TEST(ChannelTest, DISABLED_generatorWriteReadBench) {
  static constexpr int N_iter = 1000000;

  auto reader = [](Channel<int> &chan) -> Promise<void> {
    MultiPromise<int> gen = chan.getAll();
    for (int i = 1; i < N_iter; ++i) {
      EXPECT_EQ(i, co_await gen);
    }
  };

  auto writer = [](Channel<int> &chan) -> Promise<void> {
    for (int i = 1; i < N_iter; ++i) {
      co_await chan.put(i);
    }
  };

  auto setup = [&](const Loop &) -> Promise<void> {
    Channel<int> chan{2};

    Promise<void> readerCoroutine = reader(chan);
    Promise<void> writerCoroutine = writer(chan);

    co_await readerCoroutine;
    co_await writerCoroutine;
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
    drainer.schedule();

    Promise<void> put1 = chan.put(1);
    Promise<void> put1b;
    // Test copy and move assignment.
    put1b = std::move(put1);

    co_await put1b;
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

  auto source = [](Channel<int> &chan, int numIters) -> Promise<void> {
    for (int i = 1; i < numIters + 1; ++i) {
      co_await chan.put(i);
    }
  };

  bool reachedEnd = false;
  auto setup = [&](const Loop &) -> Promise<void> {
    Channel<int> chan{2};
    constexpr static int N_iter = 10;

    Promise<void> sourcer = source(chan, N_iter);
    sourcer.schedule();

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
      throw e;
    }
  };

  bool reachedEnd = false;
  auto setup = [&](const Loop &) -> Promise<void> {
    Channel<int> chan{2, 1};

    Promise<void> prom1 = reader(chan);
    Promise<void> prom2 = reader(chan);

    prom1.schedule();
    prom2.schedule();

    co_await yield();
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

TEST(ChannelTest, channelGenerator) {
  auto writer = [](Channel<int> &chan, int numIters) -> Promise<void> {
    for (int i = 0; i < numIters; ++i) {
      co_await chan.put(i);
    }
  };

  constexpr static int numIters = 10;
  int counter = 0;

  auto setup = [&](const Loop &) -> Promise<void> {
    Channel<int> chan{2};

    Promise<void> writerCoroutine = writer(chan, numIters);
    MultiPromise<int> gen = chan.getAll();

    writerCoroutine.schedule();

    for (int i = 0; i < numIters; ++i) {
      EXPECT_EQ(counter = (co_await gen).value(), i);
    }
  };

  run_loop(setup);
  EXPECT_EQ(counter, numIters - 1);
}
