
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <uv.h>

#include "exception.h"
#include "loop/loop.h"
#include "pipe.h"
#include "promise/promise.h"
#include "stream.h"
#include "test_util.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace uvco;

TEST(TtyTest, stdioTest) {
  uint64_t counter = 0;
  auto setup = [&counter](const Loop &loop) -> uvco::Promise<void> {
    std::vector<TtyStream> ttys;
    ttys.emplace_back(TtyStream::stdin(loop));
    ttys.emplace_back(TtyStream::stdout(loop));
    ttys.emplace_back(TtyStream::stderr(loop));

    for (auto &tty : ttys) {
      co_await tty.write(" ");
      ++counter;
      co_await tty.close();
      ++counter;
    }
  };

  run_loop(setup);
  EXPECT_EQ(counter, 6);
}

TEST(TtyTest, stdoutNoClose) {
  uint64_t counter = 0;
  const uv_tty_t *underlying{};
  auto setup = [&counter,
                &underlying](const Loop &loop) -> uvco::Promise<void> {
    TtyStream stdout = TtyStream::stdout(loop);
    underlying = (uv_tty_t *)stdout.underlying();

    co_await stdout.write(" ");
    ++counter;
  };

  run_loop(setup);
  EXPECT_EQ(counter, 1);

  // This test checks what happens if a coroutine finishes without closing the
  // stream. In order to satisfy asan, we still need to free the memory in the
  // end.
  delete underlying;
}

TEST(TtyTest, invalidFd) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    EXPECT_THROW({ TtyStream tty = TtyStream::tty(loop, -1); }, UvcoException);
    co_return;
  };

  run_loop(setup);
}

TEST(TtyTest, closeWhileReading) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    TtyStream tty = TtyStream::stdin(loop);
    auto reader = tty.read();
    co_await tty.close();
    EXPECT_FALSE((co_await reader).has_value());
  };

  run_loop(setup);
}

TEST(PipeTest, pipePingPong) {
  auto setup = [&](const Loop &loop) -> uvco::Promise<void> {
    auto [read, write] = pipe(loop);

    co_await write.write("Hello\n");
    co_await write.write("Hello");
    EXPECT_EQ(co_await read.read(), std::make_optional("Hello\nHello"));
    co_await read.close();
    co_await write.close();
  };

  run_loop(setup);
}

} // namespace
