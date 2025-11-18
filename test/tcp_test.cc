
#include <boost/assert.hpp>
#include <gtest/gtest.h>

#include "test_util.h"
#include "uvco/exception.h"
#include "uvco/name_resolution.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"
#include "uvco/tcp.h"
#include "uvco/tcp_stream.h"

#include <optional>
#include <string>
#include <utility>

namespace {
using namespace uvco;

Promise<void> echoReceived(TcpStream stream, bool &received, bool &responded) {
  std::optional<std::string> chunk = co_await stream.read();
  BOOST_ASSERT(chunk);
  received = true;
  co_await stream.writeBorrowed(*chunk);
  responded = true;
  co_await stream.shutdown();
  stream.close();
}

Promise<void> echoTcpServer(const Loop &loop, bool &received, bool &responded) {
  AddressHandle addr{"127.0.0.1", 8090};
  TcpServer server{loop, addr};

  MultiPromise<TcpStream> clients = server.listen();

  std::optional<TcpStream> maybeClient = co_await clients;
  BOOST_ASSERT(maybeClient);

  TcpStream client{nullptr};
  client = std::move(*maybeClient);

  client.keepAlive(true);
  client.noDelay(true);

  AddressHandle peer = client.getPeerName();
  AddressHandle ours = client.getSockName();

  EXPECT_EQ(ours.toString(), "127.0.0.1:8090");
  EXPECT_EQ(peer.address(), "127.0.0.1");

  Promise<void> clientLoop =
      echoReceived(std::move(client), received, responded);
  co_await clientLoop;
}

Promise<void> sendTcpClient(const Loop &loop, bool &sent,
                            bool &responseReceived) {
  // Also test move ctors.
  TcpClient client{loop, "127.0.0.1", 8090};
  TcpClient client2{std::move(client)};
  client = std::move(client2);

  TcpStream stream = co_await client.connect();

  co_await stream.write("Hello World");
  sent = true;
  std::optional<std::string> response = co_await stream.read();
  responseReceived = true;

  EXPECT_EQ(response, "Hello World");
  stream.close();
}

Promise<void> join(Promise<void> promise1, Promise<void> promise2) {
  co_await promise1;
  co_await promise2;
}

} // namespace

TEST(TcpTest, singlePingPong) {
  bool sent = false;
  bool received = false;
  bool responded = false;
  bool responseReceived = false;

  auto setup = [&](const Loop &loop) -> Promise<void> {
    return join(echoTcpServer(loop, received, responded),
                sendTcpClient(loop, sent, responseReceived));
  };

  run_loop(setup);
  EXPECT_TRUE(sent);
  EXPECT_TRUE(received);
  EXPECT_TRUE(responded);
  EXPECT_TRUE(responseReceived);
}

Promise<void> serverLoop(MultiPromise<TcpStream> clients) {
  while (true) {
    std::optional<TcpStream> maybeClient = co_await clients;
    if (!maybeClient) {
      co_return;
    }
    TcpStream client{nullptr};
    client = std::move(*maybeClient);

    std::optional<std::string> chunk = co_await client.read();
    BOOST_ASSERT(chunk);
    co_await client.writeBorrowed(*chunk);
    co_await client.shutdown();
    client.close();
  }
}

Promise<void> sendReceivePing(const Loop &loop, AddressHandle addr) {
  TcpClient client{loop, addr};
  TcpStream stream = co_await client.connect();

  co_await stream.write("Ping");
  std::optional<std::string> response = co_await stream.read();

  EXPECT_EQ(response, "Ping");
  stream.close();
}

TEST(TcpTest, repeatedConnectSingleServerCancel1) {
  auto setup = [&](const Loop &loop) -> Promise<void> {
    AddressHandle addr{"127.0.0.1", 0};
    TcpServer server{loop, addr};
    const AddressHandle actual = server.getSockname();
    EXPECT_LT(0, actual.port());

    Promise<void> serverHandler = serverLoop(server.listen());
    co_await sendReceivePing(loop, actual);
    co_await sendReceivePing(loop, actual);
    co_await sendReceivePing(loop, actual);

    co_return;
  };
  run_loop(setup);
}

TEST(TcpTest, repeatedConnectSingleServerCancel2) {
  auto setup = [&](const Loop &loop) -> Promise<void> {
    AddressHandle addr{"127.0.0.1", 0};
    TcpServer server{loop, addr};
    const AddressHandle actual = server.getSockname();
    EXPECT_LT(0, actual.port());

    {
      Promise<void> serverHandler = serverLoop(server.listen());
      co_await sendReceivePing(loop, actual);
      co_await sendReceivePing(loop, actual);
      co_await sendReceivePing(loop, actual);
    }

    co_return;
  };
  run_loop(setup);
}

TEST(TcpTest, repeatedConnectSingleServerCancel3) {
  auto setup = [&](const Loop &loop) -> Promise<void> {
    AddressHandle addr{"127.0.0.1", 0};
    TcpServer server{loop, addr};
    const AddressHandle actual = server.getSockname();
    EXPECT_LT(0, actual.port());

    // Just go hog-wild, cancel everything willy-nilly
    Promise<void> serverHandler = serverLoop(server.listen());
    co_await sendReceivePing(loop, actual);
    co_await sendReceivePing(loop, actual);
    sendReceivePing(loop, actual);
    co_return;
  };
  run_loop(setup);
}

TEST(TcpTest, repeatedConnectSingleServerCancel4) {
  auto setup = [&](const Loop &loop) -> Promise<void> {
    AddressHandle addr{"127.0.0.1", 0};
    TcpServer server{loop, addr};
    const AddressHandle actual = server.getSockname();
    EXPECT_LT(0, actual.port());

    // Just go hog-wild, cancel everything willy-nilly
    Promise<void> serverHandler = serverLoop(server.listen());
    co_await sendReceivePing(loop, actual);
    co_await sendReceivePing(loop, actual);
    server.close();
    EXPECT_THROW({ co_await sendReceivePing(loop, actual); }, UvcoException);
    co_return;
  };
  run_loop(setup);
}

TEST(TcpTest, validBind) {
  auto setup = [&](const Loop &loop) -> Promise<void> {
    AddressHandle addr{"127.0.0.1", 0};
    TcpServer server{loop, addr};
    const AddressHandle actual = server.getSockname();
    EXPECT_LT(0, actual.port());
    co_return;
  };

  run_loop(setup);
}

TEST(TcpTest, dropListeningServer) {
  auto setup = [&](const Loop &loop) -> Promise<void> {
    AddressHandle addr{"127.0.0.1", 0};
    TcpServer server{loop, addr};
    const AddressHandle actual = server.getSockname();
    EXPECT_LT(0, actual.port());
    MultiPromise<TcpStream> listen = server.listen();
    co_return;
  };
  run_loop(setup);
}

TEST(TcpTest, invalidLocalhostConnect) {
  auto main = [](const Loop &loop) -> Promise<void> {
    const AddressHandle addr{"::1", 39856};
    TcpClient client{loop, addr};
    EXPECT_THROW(
        {
          TcpStream stream = co_await client.connect();
          stream.close();
        },
        UvcoException);
    co_return;
  };

  run_loop(main);
}

TEST(TcpTest, dropConnect) {
  auto setup = [&](const Loop &loop) -> Promise<void> {
    TcpServer server{loop, {"127.0.0.1", 0}};
    const AddressHandle actual = server.getSockname();
    EXPECT_LT(0, actual.port());
    MultiPromise<TcpStream> listen = server.listen();

    {
      TcpClient client{loop, actual};
      Promise<TcpStream> streamPromise = client.connect();
    }
    co_return;
  };

  run_loop(setup);
}

TEST(TcpTest, dropConnect2) {
  auto setup = [&](const Loop &loop) -> Promise<void> {
    TcpServer server{loop, {"127.0.0.1", 0}};
    const AddressHandle actual = server.getSockname();
    EXPECT_LT(0, actual.port());
    MultiPromise<TcpStream> listen = server.listen();

    TcpClient client{loop, actual};
    Promise<TcpStream> streamPromise = client.connect();

    co_return;
  };

  run_loop(setup);
}
