
#include "exception.h"
#include "promise/promise.h"
#include "run.h"
#include "stream.h"
#include "test_util.h"
#include "uds.h"

#include <cstdio>
#include <fmt/core.h>
#include <fmt/format.h>

#include <gtest/gtest.h>
#include <optional>

namespace {
using namespace uvco;

TEST(UdsTest, UnixStreamServer) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    UnixStreamServer server{loop, "/tmp/uvco_test.sock"};
    try {
      std::optional<UnixStream> stream = co_await server.listen();
      if (!stream) {
        fmt::print(stderr, "No stream\n");
        co_return;
      }
      fmt::print(stderr, "Got stream at {}\n", stream->getSockName());
      fmt::print(stderr, "Got peer at {}\n", stream->getPeerName());
      co_await stream->write("Hello, world!\n");
      co_await stream->close();
    } catch (const UvcoException &e) {
      fmt::print(stderr, "Error: {}\n", e.what());
    }
    fmt::print(stderr, "Closing server\n");
    co_await server.close();
    fmt::print(stderr, "Listen finished\n");
  };

  run_loop(setup);
}

} // namespace
