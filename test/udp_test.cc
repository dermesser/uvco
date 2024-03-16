
#include "exception.h"
#include "name_resolution.h"
#include "promise/multipromise.h"
#include "promise/promise.h"
#include "timer.h"
#include "udp.h"

#include "test_util.h"

#include <cstdint>
#include <cstdio>
#include <gtest/gtest.h>

namespace {
using namespace uvco;

Promise<void> udpServer(const Loop &loop, uint64_t &received) {
  Udp server{loop};
  co_await server.bind("::1", 9999, 0);

  MultiPromise<std::pair<std::string, AddressHandle>> packets =
      server.receiveMany();

  uint32_t counter = 0;
  while (counter < 10) {
    auto recvd = co_await packets;
    if (!recvd) {
      break;
    }
    ++received;
    const std::string &buffer = recvd->first;
    auto &from = recvd->second;
    co_await server.send(buffer, from);
    ++counter;
  }
  EXPECT_EQ(server.getSockname().toString(), "[::1]:9999");
  // Necessary for the receiver promise to return and not leak memory!
  server.stopReceiveMany();
  co_await server.close();
  co_return;
}

Promise<void> udpClient(const Loop &loop, uint64_t &sent) {
  // Ensure server has started.
  co_await sleep(loop, 10);
  constexpr static uint32_t max = 10;
  // Cannot be const due to mismatch with C library some layers down.
  const std::string msg = "Hello there!";

  // Ticker stopped automatically after `max` ticks.
  auto ticker = tick(loop, 10, max);
  MultiPromise<uint64_t> tickerPromise = ticker->ticker();

  Udp client{loop};

  // Before any operation: EBADF.
  EXPECT_THROW({ client.getPeername().value(); }, UvcoException);
  EXPECT_THROW({ client.getSockname().family(); }, UvcoException);

  co_await client.bind("::1", 7777);

  EXPECT_FALSE(client.getPeername());

  co_await client.connect("::1", 9999);

  EXPECT_TRUE(client.getPeername());
  EXPECT_EQ(client.getPeername()->toString(), "[::1]:9999");

  for (uint32_t i = 0; i < max; ++i) {
    co_await tickerPromise;
    co_await client.send(msg, {});
    ++sent;
    auto response = co_await client.receiveOne();
  }

  co_await client.close();
  co_return;
}

Promise<void> join(Promise<void> promise1, Promise<void> promise2) {
  co_await promise1;
  co_await promise2;
}

} // namespace

TEST(UdpTest, testPingPong) {
  uint64_t sent = 0;
  uint64_t received = 0;
  auto setup = [&](const Loop &loop) -> uvco::Promise<void> {
    return join(udpServer(loop, received), udpClient(loop, sent));
  };

  run_loop(setup);
  EXPECT_EQ(sent, received);
  EXPECT_EQ(sent, 10);
}

TEST(UdpTest, testTtl) {
  auto setup = [&](const Loop &loop) -> uvco::Promise<void> {
    Udp server{loop};
    co_await server.bind("::1", 9999, 0);
    server.setTtl(10);
    co_await server.close();
  };

  run_loop(setup);
}

TEST(UdpTest, testBroadcast) {
  auto setup = [&](const Loop &loop) -> uvco::Promise<void> {
    Udp server{loop};
    co_await server.bind("::1", 9999);
    server.setBroadcast(true);
    try {
      std::vector<char> buf(10, 'a');
      co_await server.send(buf, AddressHandle{"255.255.255.255", 9988});
    } catch (const UvcoException &e) {
      fmt::print(stderr, "Caught exception: {}\n", e.what());
    }
    co_await server.close();
  };

  run_loop(setup);
}
