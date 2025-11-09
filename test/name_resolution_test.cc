
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <uv.h>

#include "test_util.h"

#include "uvco/exception.h"
#include "uvco/loop/loop.h"
#include "uvco/name_resolution.h"
#include "uvco/promise/promise.h"

#include <array>
#include <cstdint>

namespace {

using namespace uvco;

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

TEST(NameResolutionTest, sockaddr) {
  AddressHandle ah4{"127.0.0.1", 1234};
  const struct sockaddr *sa4 = ah4.sockaddr();
  EXPECT_EQ(sa4->sa_family, AF_INET);
  const struct sockaddr_in *sin4 = (const struct sockaddr_in *)sa4;
  EXPECT_EQ(ntohl(sin4->sin_addr.s_addr), 0x7f000001);
  EXPECT_EQ(ntohs(sin4->sin_port), 1234);
}

TEST(NameResolutionTest, fromSockaddr) {
  AddressHandle ah4{"127.0.0.1", 1234};
  AddressHandle ah6{"::1", 1234};

  const struct sockaddr *sa4 = ah4.sockaddr();
  EXPECT_EQ(sa4->sa_family, AF_INET);

  const struct sockaddr *sa6 = ah6.sockaddr();
  EXPECT_EQ(sa6->sa_family, AF_INET6);

  AddressHandle ah4_2{sa4};
  EXPECT_EQ(ah4.toString(), "127.0.0.1:1234");
  EXPECT_EQ(ah4_2.toString(), "127.0.0.1:1234");

  AddressHandle ah6_2{sa6};
  EXPECT_EQ(ah6.toString(), "[::1]:1234");
  EXPECT_EQ(ah6_2.toString(), "[::1]:1234");

  struct sockaddr saFake = *sa6;
  saFake.sa_family = 1234;
  EXPECT_THROW({ AddressHandle ahInvalid{&saFake}; }, UvcoException);
}

TEST(NameResolutionTest, resolveGoogleDns) {
  auto setup = [&](const Loop &loop) -> uvco::Promise<void> {
    Resolver resolver{loop};
    for (const auto af : {AF_INET, AF_INET6}) {
      Promise<AddressHandle> ahPromise = resolver.gai("dns.google", 443, af);
      AddressHandle address = co_await ahPromise;
      EXPECT_EQ(address.port(), 443);
      // Let's hope Google doesn't change these!
      EXPECT_TRUE(af != AF_INET || address.address().starts_with("8.8."));
      EXPECT_TRUE(af != AF_INET6 || address.address().starts_with("2001:4860"));
    }
  };

  run_loop(setup);
}

TEST(NameResolutionTest, cancelResolve) {
  auto setup = [&](const Loop &loop) -> uvco::Promise<void> {
    Resolver resolver{loop};
    Promise<AddressHandle> ahPromise = resolver.gai("google.com", 443, AF_INET);
    // drop ahPromise here
    co_return;
  };

  run_loop(setup);
}

} // namespace
