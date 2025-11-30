#include "uvco/examples/http_server.h"
#include "uvco/combinators.h"
#include "uvco/promise/promise.h"
#include "uvco/tcp.h"
#include "uvco/tcp_stream.h"

#include <boost/algorithm/string/predicate.hpp>
#include <fmt/format.h>
#include <utility>

#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>

namespace uvco::examples {

HttpResponse::HttpResponse() { headers.reserve(8); }

std::string HttpResponse::toString() {
  headers.emplace_back("Content-Length", std::to_string(body.length()));
  fmt::memory_buffer buf;
  fmt::format_to(std::back_inserter(buf), "HTTP/1.1 {} {}\r\n", statusCode,
                 statusMessage);
  for (const auto &header : headers) {
    fmt::format_to(std::back_inserter(buf), "{}: {}\r\n", header.first,
                   header.second);
  }
  fmt::format_to(std::back_inserter(buf), "\r\n");
  fmt::format_to(std::back_inserter(buf), "{}", body);
  return fmt::to_string(buf);
}

void Router::addRoute(const std::string &path, HttpHandler handler) {
  routes_[path] = std::move(handler);
}

HttpHandler Router::getHandler(const std::string &path) const {
  const auto it = routes_.find(path);
  if (it != routes_.end()) {
    return it->second;
  }
  return nullptr;
}

Promise<HttpResponse> notFoundHandler(const HttpRequest & /*request*/) {
  HttpResponse response;
  response.statusCode = 404;
  response.statusMessage = "Not Found";
  response.body = "Not Found";
  co_return response;
}

namespace {
const size_t MAX_HEADER_SIZE = 8192; // 8KB

Promise<std::optional<std::string>> readHeaders(TcpStream &stream) {
  std::string requestBuffer;
  char buffer[4096];
  size_t headersEndPos = std::string::npos;

  while (headersEndPos == std::string::npos) {
    if (requestBuffer.length() > MAX_HEADER_SIZE) {
      co_return std::nullopt;
    }
    auto bytesRead = co_await stream.read(std::span(buffer));
    if (bytesRead == 0) {
      co_return std::nullopt;
    }
    requestBuffer.append(buffer, bytesRead);
    headersEndPos = requestBuffer.find("\r\n\r\n");
  }

  co_return requestBuffer.substr(0, headersEndPos + 4);
}

std::optional<std::tuple<std::string_view, std::string_view, std::string_view>>
parseRequestLine(std::string_view requestLine) {
  const size_t methodEnd = requestLine.find(' ');
  if (methodEnd == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view method = requestLine.substr(0, methodEnd);
  requestLine.remove_prefix(methodEnd + 1);

  const size_t pathEnd = requestLine.find(' ');
  if (pathEnd == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view path = requestLine.substr(0, pathEnd);
  const std::string_view version = requestLine.substr(pathEnd + 1);

  if (method.empty() || path.empty() || version.empty()) {
    return std::nullopt;
  }

  return std::make_tuple(method, path, version);
}

void parseHeaders(std::string_view headersBlock, HttpRequest &request) {
  size_t start = 0;
  while (start < headersBlock.size()) {
    const size_t end = headersBlock.find("\r\n", start);
    if (end == std::string_view::npos) {
      break;
    }
    const std::string_view headerLine = headersBlock.substr(start, end - start);
    if (headerLine.empty()) {
      start = end + 2;
      continue;
    }
    const size_t colonPos = headerLine.find(": ");
    if (colonPos != std::string_view::npos) {
      request.headers.emplace_back(headerLine.substr(0, colonPos),
                                   headerLine.substr(colonPos + 2));
    }
    start = end + 2;
  }
}

std::optional<HttpRequest> parseRequest(std::string_view headersSection) {
  const size_t requestLineEnd = headersSection.find("\r\n");
  if (requestLineEnd == std::string_view::npos) {
    return std::nullopt;
  }

  const std::optional<
      std::tuple<std::string_view, std::string_view, std::string_view>>
      requestLineParts =
          parseRequestLine(headersSection.substr(0, requestLineEnd));
  if (!requestLineParts) {
    return std::nullopt;
  }

  HttpRequest request;
  std::tie(request.method, request.path, request.version) = *requestLineParts;

  const std::string_view headersBlock =
      headersSection.substr(requestLineEnd + 2);
  parseHeaders(headersBlock, request);

  return request;
}

Promise<void> handleBadRequest(TcpStream &stream) {
  HttpResponse response;
  response.statusCode = 400;
  response.statusMessage = "Bad Request";
  response.body = "Bad Request";
  co_await stream.write(response.toString());
  stream.close();
}

} // namespace

Promise<void> connectionHandler(TcpStream stream, const Router &router) {
  while (true) {
    const std::optional<std::string> headersStrOpt =
        co_await readHeaders(stream);
    if (!headersStrOpt) {
      co_await handleBadRequest(stream);
      co_return;
    }

    const std::optional<HttpRequest> requestOpt = parseRequest(*headersStrOpt);
    if (!requestOpt) {
      co_await handleBadRequest(stream);
      co_return;
    }

    const HttpRequest &request = *requestOpt;

    bool closeConnection = true;
    for (const auto &header : request.headers) {
      if (boost::iequals(header.first, "connection") &&
          boost::iequals(header.second, "keep-alive")) {
        closeConnection = false;
        break;
      }
    }

    HttpResponse response;
    const HttpHandler handler = router.getHandler(std::string(request.path));
    if (handler) {
      response = co_await handler(request);
    } else {
      response = co_await notFoundHandler(request);
    }

    if (closeConnection) {
      response.headers.emplace_back("Connection", "close");
    } else {
      response.headers.emplace_back("Connection", "keep-alive");
    }

    co_await stream.write(response.toString());

    if (closeConnection) {
      stream.close();
      co_return;
    }
  }
}

Promise<void> httpServer(TcpServer &server, Router router) {
  auto streamPromise = server.listen();

  auto handlers = TaskSet::create();
  while (auto stream = co_await streamPromise.next()) {
    handlers->add(connectionHandler(std::move(*stream), router));
  }
}

} // namespace uvco::examples
