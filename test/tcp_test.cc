
#include "promise/multipromise.h"
#include "promise/promise.h"
#include "tcp.h"

#include "test_util.h"

#include <gtest/gtest.h>

namespace {
using namespace uvco;

Promise<void> echoReceived(TcpStream stream, bool &received, bool &responded) {
  std::optional<std::string> chunk = co_await stream.read();
  BOOST_ASSERT(chunk);
  received = true;
  co_await stream.write(std::move(*chunk));
  responded = true;
  co_await stream.closeReset();
}

Promise<void> echoTcpServer(uv_loop_t *loop, bool &received, bool &responded) {
  AddressHandle addr{"127.0.0.1", 8090};
  TcpServer server{loop, addr};

  MultiPromise<TcpStream> clients = server.listen();

  std::optional<TcpStream> client = co_await clients;
  BOOST_ASSERT(client);

  client->keepAlive(true);
  client->noDelay(true);

  AddressHandle peer = client->getPeerName();
  AddressHandle ours = client->getSockName();

  EXPECT_EQ(ours.toString(), "127.0.0.1:8090");
  EXPECT_EQ(peer.address(), "127.0.0.1");

  Promise<void> clientLoop =
      echoReceived(std::move(*client), received, responded);
  co_await clientLoop;
  co_await server.close();
}

Promise<void> sendTcpClient(uv_loop_t *loop, bool &sent,
                            bool &responseReceived) {
  TcpClient client{loop, "127.0.0.1", 8090};
  TcpClient client2{std::move(client)};

  TcpStream stream = co_await client2.connect();

  co_await stream.write("Hello World");
  sent = true;
  std::optional<std::string> response = co_await stream.read();
  responseReceived = true;

  EXPECT_EQ(response, "Hello World");
  co_await stream.close();
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

  auto setup = [&](uv_loop_t *loop) -> uvco::Promise<void> {
    return join(echoTcpServer(loop, received, responded),
                sendTcpClient(loop, sent, responseReceived));
  };

  run_loop(setup);
  EXPECT_TRUE(sent);
  EXPECT_TRUE(received);
  EXPECT_TRUE(responded);
  EXPECT_TRUE(responseReceived);
}

TEST(TcpTest, validBind) {
  auto setup = [&](uv_loop_t *loop) -> uvco::Promise<void> {
    AddressHandle addr{"127.0.0.1", 0};
    TcpServer server{loop, addr};
    co_await server.close();
  };

  run_loop(setup);
}
