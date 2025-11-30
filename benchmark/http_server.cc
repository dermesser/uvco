// uvco (c) 2025 Lewin Bormann. See LICENSE for specific terms.

#include <boost/program_options.hpp>
#include <fmt/core.h>

#include "uvco/examples/http_server.h"
#include "uvco/name_resolution.h"
#include "uvco/run.h"
#include "uvco/tcp.h"

#include <iostream>
#include <string>

using namespace boost::program_options;
using namespace uvco;
using namespace uvco::examples;

namespace {

// Example handlers
Promise<HttpResponse> handleRoot(const HttpRequest & /*request*/) {
  HttpResponse response;
  response.body = "Hello World!";
  co_return response;
}

Promise<HttpResponse> handleHello(const HttpRequest & /*request*/) {
  HttpResponse response;
  response.body = "Hello";
  co_return response;
}

} // namespace

int main(int argc, char *argv[]) {
  options_description desc(
      "Allowed options for the http_server benchmark binary. This executable "
      "can be used for somewhat realistic throughput tests and profiling of "
      "the uvco library, especially in conjunction with tools like 'wrk' and "
      "'ab'. It installs handlers for the / and /hello paths, "
      "returning short strings and a minimal response for each. If "
      "'Connection: keep-alive' is sent in the request, this server supports "
      "handling multiple requests on one connection");
  // clang-format off
  desc.add_options()
    ("help", "produce help message")
    ("port", value<uint16_t>()->default_value(8000), "port to listen on")
    ("address", value<std::string>()->default_value("0.0.0.0"), "bind address");
  // clang-format on

  variables_map varMap;
  store(parse_command_line(argc, argv, desc), varMap);
  notify(varMap);

  if (varMap.contains("help")) {
    std::cout << desc << "\n";
    return 1;
  }

  const auto port = varMap["port"].as<uint16_t>();

  Router router;
  router.addRoute("/", handleRoot);
  router.addRoute("/hello", handleHello);

  runMain<void>([&](const Loop &loop) -> Promise<void> {
    Resolver resolver(loop);
    auto address = co_await resolver.gai("::", port, AF_INET6);

    TcpServer server(loop, address);

    fmt::print("Bound to {}, server ready\n", address.toString());
    co_await httpServer(server, std::move(router));
  });

  return 0;
}
