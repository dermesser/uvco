
#include <boost/assert.hpp>
#include <gtest/gtest.h>
#include <uv.h>

#include "test_util.h"
#include "uvco/exception.h"
#include "uvco/loop/loop.h"
#include "uvco/pipe.h"
#include "uvco/promise/promise.h"
#include "uvco/stream.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace uvco;

// Doesn't work on Github Actions because no TTY is attached.
TEST(TtyTest, DISABLED_stdioTest) {
  uint64_t counter = 0;
  auto setup = [&counter](const Loop &loop) -> Promise<void> {
    std::vector<TtyStream> ttys;
    ttys.emplace_back(TtyStream::stdin(loop));
    ttys.emplace_back(TtyStream::stdout(loop));
    ttys.emplace_back(TtyStream::stderr(loop));

    for (auto &tty : ttys) {
      co_await tty.write(" ");
      ++counter;
      tty.close();
      ++counter;
    }
  };

  run_loop(setup);
  EXPECT_EQ(counter, 6);
}

TEST(TtyTest, stdoutNoClose) {
  uint64_t counter = 0;
  auto setup = [&](const Loop &loop) -> Promise<void> {
    TtyStream stdout = TtyStream::stdout(loop);

    co_await stdout.write(" ");
    ++counter;
  };

  run_loop(setup);
  EXPECT_EQ(counter, 1);
}

TEST(TtyTest, invalidFd) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    EXPECT_THROW({ TtyStream tty = TtyStream::tty(loop, -1); }, UvcoException);
    co_return;
  };

  run_loop(setup);
}

TEST(TtyTest, closeWhileReading) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    TtyStream tty = TtyStream::stdin(loop);
    Promise<std::optional<std::string>> reader = tty.read();
    tty.close();
    EXPECT_FALSE((co_await reader).has_value());
  };

  run_loop(setup);
}

TEST(PipeTest, pipePingPong) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    auto [read, write] = pipe(loop);

    co_await write.write("Hello\n");
    co_await write.write("Hello");
    EXPECT_EQ(co_await read.read(), std::make_optional("Hello\nHello"));
    read.close();
    write.close();
  };

  run_loop(setup);
}

TEST(PipeTest, doubleReadDies) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    auto [read, write] = pipe(loop);

    Promise<std::optional<std::string>> readPromise = read.read();
    EXPECT_THROW({ co_await read.read(); }, UvcoException);
    read.close();
    write.close();
  };

  run_loop(setup);
}

TEST(PipeTest, largeWriteRead) {
  std::ifstream urandom("/dev/urandom", std::ios::binary);
  std::array<char, 1024> buffer{};
  urandom.read(buffer.data(), buffer.size());

  auto setup = [&](const Loop &loop) -> Promise<void> {
    auto [read, write] = pipe(loop);

    for (unsigned i = 0; i < 10; ++i) {
      EXPECT_EQ(buffer.size(), co_await write.write(
                                   std::string(buffer.data(), buffer.size())));
    }
    write.close();

    [[maybe_unused]]
    size_t bytesRead{};

    while (true) {
      // Read random chunk size.
      auto chunk = co_await read.read(732);
      if (!chunk.has_value()) {
        break;
      }
      bytesRead += chunk->size();
    }
    BOOST_ASSERT(bytesRead == 10240);
    read.close();
  };

  run_loop(setup);
}

TEST(PipeTest, readIntoBuffer) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    auto [read, write] = pipe(loop);

    co_await write.write("Hello");
    std::array<char, 32> buffer{};
    size_t bytesRead = co_await read.read(buffer);
    EXPECT_EQ(bytesRead, 5);
    EXPECT_EQ(std::string(buffer.data(), bytesRead), "Hello");
    read.close();
    write.close();
  };
  run_loop(setup);
}

} // namespace
