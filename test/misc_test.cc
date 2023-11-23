
#include "internal_utils.h"
#include "promise.h"
#include "tcp.h"

#include "test_util.h"

#include <gtest/gtest.h>

namespace {
  using namespace uvco;
}

TEST(NameResolutionTest, resolveGoogleDotCom) {
  auto setup = [&](uv_loop_t *loop) -> uvco::Promise<void> {
    Resolver resolver{loop};
    Promise<AddressHandle> ahPromise = resolver.gai("dns.google", 443, AF_INET);
    AddressHandle address = co_await ahPromise;
    EXPECT_EQ(address.port(), 443);
    EXPECT_TRUE(address.address().starts_with("8.8."));
  };

  run_loop(setup);
}

TEST(TtyTest, stdinTest) {
  uint64_t counter = 0;
  auto setup = [&](uv_loop_t *loop) -> uvco::Promise<void> {
    TtyStream stdin = TtyStream::stdin(loop);

    co_await stdin.write(" ");
    ++counter;
    co_await stdin.close();
    ++counter;
  };

  run_loop(setup);
  EXPECT_EQ(counter, 2);
}
