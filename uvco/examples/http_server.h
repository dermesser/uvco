// uvco (c) 2025 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/core.h>
#include <fmt/format.h>

#include "uvco/promise/promise.h"
#include "uvco/tcp.h"

namespace uvco::examples {

/// @addtogroup examples
/// @{
/// Simple HTTP server implementation using uvco coroutines and promises.

struct HttpRequest {
  std::string_view method;
  std::string_view path;
  std::string_view version;
  std::vector<std::pair<std::string_view, std::string_view>> headers;
};

struct HttpResponse {
  int statusCode = 200;
  std::string statusMessage = "OK";
  std::vector<std::pair<std::string_view, std::string>> headers;
  std::string body;

  HttpResponse();

  std::string toString();
};

using HttpHandler = std::function<Promise<HttpResponse>(const HttpRequest &)>;

/// A simple HTTP router mapping paths to handlers. Supplied to the httpServer()
/// function to dispatch requests to handlers.
class Router {
public:
  void addRoute(const std::string &path, HttpHandler handler);

  [[nodiscard]] HttpHandler getHandler(const std::string &path) const;

private:
  std::unordered_map<std::string, HttpHandler> routes_;
};

/// A handler returning a 404 error.
Promise<HttpResponse> notFoundHandler(const HttpRequest &request);

/// Run a simple HTTP server on the given TcpServer, serving the routes defined
/// by the supplied Router instance. Never returns, unless an exception is
/// thrown while accepting a connection or spawning a task.
Promise<void> httpServer(TcpServer &server, Router router);

/// @}

} // namespace uvco::examples
