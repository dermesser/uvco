
#include <uv.h>

#include "exception.h"
#include "loop/loop.h"
#include "name_resolution.h"
#include "promise/multipromise.h"
#include "promise/promise.h"
#include "timer.h"
#include "udp.h"

#include "test_util.h"

#include <cstdint>
#include <cstdio>
#include <fmt/core.h>
#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace uvco;

constexpr uint32_t pingPongCount = 100;

Promise<void> udpServer(const Loop &loop, uint64_t &received) {
  Udp server{loop};
  co_await server.bind("::1", 9999, 0);

  MultiPromise<std::pair<std::string, AddressHandle>> packets =
      server.receiveMany();

  for (uint32_t counter = 0; counter < pingPongCount; ++counter) {
    auto recvd = co_await packets;
    if (!recvd) {
      break;
    }
    ++received;
    std::string &buffer = recvd->first;
    auto &from = recvd->second;
    co_await server.send(buffer, from);
  }
  EXPECT_EQ(server.getSockname().toString(), "[::1]:9999");
  // Necessary for the receiver promise to return and not leak memory!
  server.stopReceiveMany(packets);
  EXPECT_FALSE((co_await packets).has_value());
  co_await server.close();
  co_return;
}

Promise<void> udpClient(const Loop &loop, uint64_t &sent) {
  // Ensure server has started.
  co_await sleep(loop, 10);
  std::string msg = "Hello there!";

  Udp client{loop};

  // Before any operation: EBADF.
  EXPECT_THROW({ client.getPeername().value(); }, UvcoException);
  EXPECT_THROW({ client.getSockname().family(); }, UvcoException);

  co_await client.bind("::1", 7777);

  EXPECT_FALSE(client.getPeername());

  co_await client.connect("::1", 9999);

  // Repeat but with AddressHandle
  const AddressHandle dest{"::1", 9999};
  co_await client.connect(dest);

  EXPECT_TRUE(client.getPeername());
  EXPECT_EQ(client.getPeername()->toString(), "[::1]:9999");

  for (uint32_t i = 0; i < pingPongCount; ++i) {
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

TEST(UdpTest, testPingPong) {
  uint64_t sent = 0;
  uint64_t received = 0;
  auto setup = [&](const Loop &loop) -> uvco::Promise<void> {
    return join(udpServer(loop, received), udpClient(loop, sent));
  };

  run_loop(setup);
  EXPECT_EQ(sent, received);
  EXPECT_EQ(sent, pingPongCount);
}

TEST(UdpTest, testDropReceiver) {
  auto setup = [&](const Loop &loop) -> uvco::Promise<void> {
    Udp server{loop};
    co_await server.bind("::1", 9999, 0);

    Udp client{loop};
    co_await client.bind("::1", 7777);
    co_await client.connect("::1", 9999);

    MultiPromise<std::pair<std::string, AddressHandle>> packets =
        server.receiveMany();
    ;

    std::string fromClient{"Hello there!"};
    co_await client.send(fromClient, {});

    const auto receivedFromClient = co_await packets;
    EXPECT_TRUE(receivedFromClient.has_value());

    std::string msg = "Hello there!";
    co_await server.send(msg, AddressHandle{"::1", 7777});
    co_await server.close();

    const auto receivedFromServer = co_await client.receiveOne();
    EXPECT_EQ(msg, receivedFromServer);

    co_await client.close();
  };

  run_loop(setup);
}

TEST(UdpTest, cancelWhileReceiving) {
  auto setup = [&](const Loop &loop) -> uvco::Promise<void> {
    Udp server{loop};
    co_await server.bind("::1", 9999, 0);

    MultiPromise<std::pair<std::string, AddressHandle>> packets =
        server.receiveMany();

    co_await sleep(loop, 1);
    server.stopReceiveMany(packets);
    co_await server.close();
  };

  run_loop(setup);
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

TEST(UdpTest, udpNoClose) {
  uint64_t counter = 0;
  const uv_udp_t *underlying{};
  auto setup = [&counter,
                &underlying](const Loop &loop) -> uvco::Promise<void> {
    Udp udp = Udp{loop};
    const AddressHandle dest{"::1", 38212};
    underlying = udp.underlying();

    std::string message = "Hello";
    co_await udp.send(message, dest);
    ++counter;
  };

  run_loop(setup);
  EXPECT_EQ(counter, 1);

  // This test checks what happens if a coroutine finishes without closing the
  // stream. In order to satisfy asan, we still need to free the memory in the
  // end.
  delete underlying;
}

TEST(UdpTest, sendNoAddress) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    Udp udp{loop};
    std::string message = "Hello";
    try {
      co_await udp.send(message, {});
      // Shouldn't reach here.
      EXPECT_FALSE(true);
    } catch (const UvcoException &e) {
      fmt::print(stderr, "Caught exception: {}\n", e.what());
    }
    co_await udp.close();
  };

  run_loop(setup);
}

TEST(UdpTest, closeWhileReceiving) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    Udp udp{loop};
    co_await udp.bind("::1", 9999);
    auto receiver = udp.receiveMany();
    co_await udp.close();
  };

  run_loop(setup);
}

} // namespace
