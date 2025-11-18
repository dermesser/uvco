// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <uv.h>

#include "test_util.h"

#include "uvco/combinators.h"
#include "uvco/exception.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"
#include "uvco/uds.h"
#include "uvco/uds_stream.h"

#include <fmt/core.h>
#include <fmt/format.h>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

namespace {
using namespace uvco;

constexpr std::string_view testSocketPath = "/tmp/_uvco_test.sock";

Promise<void> pingPongServer(const Loop &loop) {
  UnixStreamServer server{loop, testSocketPath};
  auto listener = server.listen();
  try {
    std::optional<UnixStream> stream = co_await listener;
    if (!stream) {
      fmt::print(stderr, "No stream\n");
      co_return;
    }
    fmt::print(stderr, "Server at {}\n", stream->getSockName());
    fmt::print(stderr, "Received connection from client at {}\n",
               stream->getPeerName());
    std::optional<std::string> message = co_await stream->read();
    if (!message) {
      throw UvcoException{UV_EOF, "No data from client"};
    }

    co_await stream->write(
        fmt::format("Hello, back! I received '{}'\n", message.value()));
    co_await stream->close();
  } catch (const UvcoException &e) {
    fmt::print(stderr, "Error: {}\n", e.what());
    throw;
  }

  fmt::print(stderr, "Closing server\n");
  fmt::print(stderr, "Listen finished\n");
}

Promise<void> pingPongClient(const Loop &loop) {
  UnixStreamClient client{loop};
  try {
    UnixStream stream = co_await client.connect(testSocketPath);
    fmt::print(stderr, "Client connecting from {}\n", stream.getSockName());
    fmt::print(stderr, "Client connected to server at {}\n",
               stream.getPeerName());

    co_await stream.write("Hello from client");
    fmt::print(stderr, "Sent; waiting for reply\n");

    std::optional<std::string> buf = co_await stream.read();
    if (!buf) {
      throw UvcoException{UV_EOF, "No data from server"};
    }
    fmt::print(stderr, "Received: {}", *buf);
    co_await stream.close();
  } catch (const UvcoException &e) {
    fmt::print(stderr, "Error: {}\n", e.what());
    throw;
  }
}

TEST(UdsTest, unixStreamPingPong) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    try {
      Promise<void> server = pingPongServer(loop);
      co_await yield();
      Promise<void> client = pingPongClient(loop);
      co_await server;
      co_await client;
    } catch (const UvcoException &e) {
      fmt::print(stderr, "Error in setup function: {}\n", e.what());
      throw;
    }
  };

  run_loop(setup);
}

TEST(UdsTest, dropServer) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    UnixStreamServer server{loop, testSocketPath};
    MultiPromise<UnixStream> listener = server.listen();
    co_return;
  };

  run_loop(setup);
}

TEST(UdsTest, dropPingPong) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    Promise<void> server = pingPongServer(loop);
    Promise<void> client = pingPongClient(loop);
    co_return;
  };

  run_loop(setup);
}

// The following test cases are adapted from the tcp-test suite and check that
// cancellations in various constellations work safely.

Promise<void> serverLoop(MultiPromise<UnixStream> clients) {
  while (true) {
    std::optional<UnixStream> maybeClient = co_await clients;
    if (!maybeClient) {
      co_return;
    }
    UnixStream client = std::move(*maybeClient);

    std::optional<std::string> chunk = co_await client.read();
    BOOST_ASSERT(chunk);
    co_await client.writeBorrowed(*chunk);
    co_await client.shutdown();
    co_await client.close();
  }
}

Promise<void> sendReceivePing(const Loop &loop) {
  UnixStreamClient client{loop};
  UnixStream stream = co_await client.connect(testSocketPath);

  co_await stream.write("Ping");
  std::optional<std::string> response = co_await stream.read();

  EXPECT_EQ(response, "Ping");
  co_await stream.close();
}

TEST(UdsTest, repeatedConnectSingleServerCancel1) {
  auto setup = [&](const Loop &loop) -> Promise<void> {
    UnixStreamServer server{loop, testSocketPath};

    Promise<void> serverHandler = serverLoop(server.listen());
    co_await sendReceivePing(loop);
    co_await sendReceivePing(loop);
    co_await sendReceivePing(loop);

  };
  run_loop(setup);
}

TEST(UdsTest, repeatedConnectSingleServerCancel2) {
  auto setup = [&](const Loop &loop) -> Promise<void> {
    UnixStreamServer server{loop, testSocketPath};

    {
      Promise<void> serverHandler = serverLoop(server.listen());
      co_await sendReceivePing(loop);
      co_await sendReceivePing(loop);
      co_await sendReceivePing(loop);
    }

  };
  run_loop(setup);
}

TEST(UdsTest, repeatedConnectSingleServerCancel3) {
  auto setup = [&](const Loop &loop) -> Promise<void> {
    UnixStreamServer server{loop, testSocketPath};

    // Just go hog-wild, cancel everything willy-nilly
    Promise<void> serverHandler = serverLoop(server.listen());
    co_await sendReceivePing(loop);
    co_await sendReceivePing(loop);
    sendReceivePing(loop);
  };
  run_loop(setup);
}

TEST(UdsTest, unixStreamFailConnect) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    try {
      UnixStreamClient client{loop};
      UnixStream stream = co_await client.connect("/tmp/does_not_exist.sock");
    } catch (const UvcoException &e) {
      EXPECT_EQ(UV_ENOENT, e.status);
      throw;
    }
  };

  EXPECT_THROW({ run_loop(setup); }, UvcoException);
}

TEST(UdsTest, unixStreamListenerStop) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    UnixStreamServer server{loop, testSocketPath};
    auto listener = server.listen();
    co_await yield();
    // The following line would result in a crash (intentional) because the
    // listener generator has returned.
    // co_await listener;
    co_return;
  };

  run_loop(setup);
}

} // namespace
