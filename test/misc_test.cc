
#include "exception.h"
#include "name_resolution.h"
#include "pipe.h"
#include "promise/promise.h"
#include "run.h"
#include "test_util.h"

#include <optional>
#include <string>

#include <gtest/gtest.h>

namespace {
using namespace uvco;
}

TEST(PromiseTest, voidImmediate) {
  auto setup = [&](const Loop &loop) -> uvco::Promise<void> {
    Promise<void> p = Promise<void>::immediate();
    co_await p;
  };

  run_loop(setup);
}

TEST(NameResolutionTest, ipv4Raw) {
  constexpr std::array<uint8_t, 4> ip4{127, 0, 0, 1};
  const auto *rawIp = (uint32_t *)ip4.data();
  AddressHandle ah4{ip4, 1234};
  EXPECT_EQ(ah4.address(), "127.0.0.1");
  EXPECT_EQ(ah4.toString(), "127.0.0.1:1234");

  AddressHandle ah4_2{*rawIp, 1234};
  EXPECT_EQ(ah4_2.toString(), "127.0.0.1:1234");
}

TEST(NameResolutionTest, ipv6Raw) {
  constexpr std::array<uint8_t, 16> ip6{0, 0, 0, 0, 0, 0, 0, 0,
                                        0, 0, 0, 0, 0, 0, 0, 1};
  AddressHandle ah6{ip6, 1234};
  EXPECT_EQ(ah6.address(), "::1");
  EXPECT_EQ(ah6.toString(), "[::1]:1234");
}

TEST(NameResolutionTest, ipInvalid) {
  constexpr std::array<uint8_t, 3> ipx{127, 0, 0};
  EXPECT_THROW({ AddressHandle ah4(ipx, 1234); }, UvcoException);
}

TEST(NameResolutionTest, ipv4Parse) {
  AddressHandle ah{"127.0.0.1", 1234};
  EXPECT_EQ(ah.address(), "127.0.0.1");
  EXPECT_EQ(ah.family(), AF_INET);
}

TEST(NameResolutionTest, ipv6Parse) {
  AddressHandle ah{"::1", 1234};
  EXPECT_EQ(ah.address(), "::1");
  EXPECT_EQ(ah.family(), AF_INET6);
}

TEST(NameResolutionTest, resolveGoogleDotCom) {
  auto setup = [&](const Loop &loop) -> uvco::Promise<void> {
    Resolver resolver{loop.uvloop()};
    Promise<AddressHandle> ahPromise = resolver.gai("dns.google", 443, AF_INET);
    AddressHandle address = co_await ahPromise;
    EXPECT_EQ(address.port(), 443);
    EXPECT_TRUE(address.address().starts_with("8.8."));
  };

  run_loop(setup);
}

TEST(TtyTest, stdoutTest) {
  uint64_t counter = 0;
  auto setup = [&counter](const Loop &loop) -> uvco::Promise<void> {
    TtyStream stdout = TtyStream::stdout(loop);

    co_await stdout.write(" ");
    ++counter;
    co_await stdout.close();
    ++counter;
  };

  run_loop(setup);
  EXPECT_EQ(counter, 2);
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
