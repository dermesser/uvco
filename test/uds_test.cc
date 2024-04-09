
#include "exception.h"
#include "promise/promise.h"
#include "run.h"
#include "test_util.h"
#include "timer.h"
#include "uds.h"

#include <cstdio>
#include <fmt/core.h>
#include <fmt/format.h>

#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <string_view>
#include <uv.h>

namespace {
using namespace uvco;

constexpr std::string_view testSocketPath = "/tmp/_uvco_test.sock";

Promise<void> pingPongServer(const Loop &loop) {
  UnixStreamServer server{loop, testSocketPath};
  try {
    std::optional<UnixStream> stream = co_await server.listen();
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
  co_await server.close();
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

TEST(UdsTest, UnixStreamPingPong) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    try {
      Promise<void> server = pingPongServer(loop);
      co_await sleep(loop, 1);
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

TEST(UdsTest, UnixStreamFailConnect) {
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

} // namespace
