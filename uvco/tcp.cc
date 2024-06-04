// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <boost/assert.hpp>
#include <fmt/core.h>
#include <fmt/format.h>
#include <sys/socket.h>
#include <uv.h>

#include "uvco/close.h"
#include "uvco/exception.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/name_resolution.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"
#include "uvco/stream_server_base.h"
#include "uvco/tcp.h"
#include "uvco/tcp_stream.h"

#include <coroutine>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace uvco {

TcpClient::TcpClient(const Loop &loop, std::string target_host_address,
                     uint16_t target_host_port, int af_hint)
    : loop_{&loop}, host_{std::move(target_host_address)}, af_hint_{af_hint},
      port_{target_host_port} {}

TcpClient::TcpClient(const Loop &loop, AddressHandle address)
    : loop_{&loop}, host_{address.address()}, af_hint_{AF_UNSPEC},
      port_{address.port()} {}

TcpClient::TcpClient(TcpClient &&other) noexcept
    : loop_{other.loop_}, host_{std::move(other.host_)},
      af_hint_{other.af_hint_}, port_{other.port_} {}

TcpClient &TcpClient::operator=(TcpClient &&other) noexcept {
  loop_ = other.loop_;
  host_ = std::move(other.host_);
  port_ = other.port_;
  return *this;
}

Promise<TcpStream> TcpClient::connect() {
  Resolver resolver{*loop_};

  AddressHandle address =
      co_await resolver.gai(host_, fmt::format("{}", port_), af_hint_);

  uv_connect_t req;
  ConnectAwaiter_ connect{};
  req.data = &connect;

  auto tcp = std::make_unique<uv_tcp_t>();

  uv_tcp_init(loop_->uvloop(), tcp.get());
  const uv_status connectStatus =
      uv_tcp_connect(&req, tcp.get(), address.sockaddr(), onConnect);
  if (connectStatus < 0) {
    // Clean up handle if connect failed.
    co_await closeHandle(tcp.get());
    throw UvcoException(connectStatus,
                        "TcpClient::connect() failed immediately");
  }

  const uv_status awaitStatus = co_await connect;
  if (awaitStatus < 0) {
    co_await closeHandle(tcp.get());
    throw UvcoException(awaitStatus, "TcpClient::connect() failed");
  }

  co_return TcpStream{std::move(tcp)};
}

void TcpClient::onConnect(uv_connect_t *req, uv_status status) {
  auto *connect = static_cast<ConnectAwaiter_ *>(req->data);
  connect->onConnect(status);
}

bool TcpClient::ConnectAwaiter_::await_ready() const {
  return status_.has_value();
}

bool TcpClient::ConnectAwaiter_::await_suspend(std::coroutine_handle<> handle) {
  BOOST_ASSERT(!handle_);
  handle_ = handle;
  return true;
}

uv_status TcpClient::ConnectAwaiter_::await_resume() {
  BOOST_ASSERT(status_);
  return *status_;
}

void TcpClient::ConnectAwaiter_::onConnect(uv_status status) {
  status_ = status;
  if (handle_) {
    Loop::enqueue(*handle_);
    handle_.reset();
  }
}

TcpServer::TcpServer(const Loop &loop, AddressHandle bindAddress, bool ipv6Only)
    : StreamServerBase{std::make_unique<uv_tcp_t>()} {
  uv_tcp_init(loop.uvloop(), socket_.get());
  const auto *sockaddr = bindAddress.sockaddr();
  const int flags = ipv6Only ? UV_TCP_IPV6ONLY : 0;
  bind(sockaddr, flags);
}

void TcpServer::bind(const struct sockaddr *addr, int flags) {
  const uv_status status = uv_tcp_bind(socket_.get(), addr, flags);
  if (status != 0) {
    throw UvcoException{status, "TcpServer::bind()"};
  }
}

} // namespace uvco
