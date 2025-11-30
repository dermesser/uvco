// uvco (c) 2025 Lewin Bormann. See LICENSE for specific terms.

#include "uvco/combinators.h"
#include "uvco/examples/http_server.h"
#include "uvco/name_resolution.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"
#include "uvco/tcp.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <utility>

using namespace uvco;
using namespace uvco::examples;

namespace {

static constexpr std::string_view serverHost = "127.0.0.1";

Promise<std::pair<TcpServer, uint16_t>> bindServer(const Loop &loop) {
  Resolver resolver(loop);
  auto address = co_await resolver.gai(serverHost, 0, AF_INET);
  TcpServer server(loop, address);
  const uint16_t boundPort = server.getSockname().port();
  co_return {std::move(server), boundPort};
}

Router defaultRoutes() {
  Router router;
  router.addRoute("/test", [](const HttpRequest &) -> Promise<HttpResponse> {
    HttpResponse res;
    res.body = "Test Response";
    co_return res;
  });
  return router;
}

TEST(HttpServerTest, SimpleRequest) {
  runMain<void>([](const Loop &loop) -> Promise<void> {
    auto [server, boundPort] = co_await bindServer(loop);

    auto clientLogic = [&]() -> Promise<void> {
      TcpClient client(loop, std::string{serverHost}, boundPort);
      auto stream = co_await client.connect();

      auto paths = std::to_array<std::string>({"/", "/test", "/notfound"});

      for (const std::string &path : paths) {

        std::string req =
            fmt::format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: "
                        "keep-alive\r\n\r\n",
                        path);
        co_await stream.write(req);

        std::array<char, 1024> buffer{};
        const size_t bytesRead = co_await stream.read(std::span(buffer));
        const std::string_view response(buffer.data(), bytesRead);

        EXPECT_TRUE(response.find("HTTP/1.1") != std::string::npos);
      }

      // Last request, send with connection-close
      co_await stream.write(
          "GET /test HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
      std::array<char, 1024> buffer{};
      const size_t bytesRead = co_await stream.read(buffer);
      const std::string_view response(buffer.data(), bytesRead);
      EXPECT_TRUE(response.find("Test Response") != std::string::npos);

      // Check that connection is closed
      EXPECT_EQ(0, co_await stream.read(buffer));

      stream.close();
    };

    co_await raceIgnore(httpServer(server, defaultRoutes()), clientLogic());
  });
}

TEST(HttpServerTest, invalidRequest) {
  runMain<void>([](const Loop &loop) -> Promise<void> {
    auto [server, boundPort] = co_await bindServer(loop);

    auto clientLogic = [&]() -> Promise<void> {
      std::array invalidRequests = std::to_array<std::string>(
          {"\r\n\r\n", "GET\r\n\r\n", "GET \r\n\r\n", "   \r\n\r\n",
           "GET\r\nHost: localhost\r\n\r\n"});

      for (const std::string &invalidRequest : invalidRequests) {
        TcpClient client(loop, std::string{serverHost}, boundPort);
        auto stream = co_await client.connect();
        co_await stream.write(invalidRequest);

        std::array<char, 1024> buffer{};
        const size_t bytesRead = co_await stream.read(buffer);
        const std::string_view response(buffer.data(), bytesRead);
        EXPECT_TRUE(response.find("400 Bad Request") != std::string::npos);

        // Check what happens if we write to the (remotely) closed stream
        EXPECT_LT(16, co_await stream.write(
                          "GET /test HTTP/1.1\r\nHost: localhost\r\n\r\n"));
        EXPECT_EQ(0, co_await stream.read(buffer));

        stream.close();
      }
    };

    co_await raceIgnore(httpServer(server, defaultRoutes()), clientLogic());
  });
}

} // namespace
