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

struct TcpClient::ConnectAwaiter_ {
  ConnectAwaiter_();
  ~ConnectAwaiter_();

  [[nodiscard]] bool await_ready() const;
  bool await_suspend(std::coroutine_handle<> handle);
  TcpStream await_resume();

  void onConnect(uv_status status);

  std::unique_ptr<uv_connect_t> req_;
  std::unique_ptr<uv_tcp_t> socket_;
  std::coroutine_handle<> handle_;
  std::optional<uv_status> status_;
};

TcpClient::ConnectAwaiter_::ConnectAwaiter_()
    : req_{std::make_unique<uv_connect_t>()},
      socket_{std::make_unique<uv_tcp_t>()} {
  setRequestData(req_.get(), this);
}

TcpClient::ConnectAwaiter_::~ConnectAwaiter_() {
  if (socket_ != nullptr) {
    closeHandle(socket_.release());
  }
  if (!requestDataIsNull(req_.get())) {
    resetRequestData(req_.get());
    uv_cancel((uv_req_t *)req_.get());
  }
}

bool TcpClient::ConnectAwaiter_::await_ready() const {
  return status_.has_value();
}

bool TcpClient::ConnectAwaiter_::await_suspend(std::coroutine_handle<> handle) {
  BOOST_ASSERT(!handle_);
  handle_ = handle;
  return true;
}

TcpStream TcpClient::ConnectAwaiter_::await_resume() {
  resetRequestData(req_.get());
  BOOST_ASSERT(status_);
  if (status_.value() != 0) {
    throw UvcoException{status_.value(),
                        "TcpClient::connect() connection failed"};
  }
  return TcpStream{std::move(socket_)};
}

void TcpClient::ConnectAwaiter_::onConnect(uv_status status) {
  status_ = status;
  if (handle_ != nullptr) {
    Loop::enqueue(handle_);
    handle_ = nullptr;
  }
}

TcpClient::TcpClient(const Loop &loop, std::string target_host_address,
                     uint16_t target_host_port, int af_hint)
    : loop_{&loop}, host_{std::move(target_host_address)}, af_hint_{af_hint},
      port_{target_host_port} {}

TcpClient::TcpClient(const Loop &loop, AddressHandle address)
    : loop_{&loop}, resolvedAddress_{address} {}

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
  AddressHandle address;
  if (resolvedAddress_.has_value()) {
    address = resolvedAddress_.value();
  } else {
    Resolver resolver{*loop_};
    address = co_await resolver.gai(host_, port_, af_hint_);
  }

  ConnectAwaiter_ connect;

  const uv_status initStatus =
      uv_tcp_init(loop_->uvloop(), connect.socket_.get());
  if (initStatus < 0) {
    throw UvcoException(initStatus,
                        "TcpClient::connect() uv_tcp_init() failed");
  }

  const uv_status connectStatus = uv_tcp_connect(
      connect.req_.get(), connect.socket_.get(), address.sockaddr(), onConnect);
  if (connectStatus < 0) {
    // Clean up handle if connect failed.
    co_await closeHandle(connect.socket_.get());
    connect.socket_.reset();
    throw UvcoException(connectStatus,
                        "TcpClient::connect() failed immediately");
  }

  std::optional<UvcoException> maybeError;
  try {
    co_return (co_await connect);
  } catch (const UvcoException &e) {
    maybeError = e;
  }

  std::unique_ptr<uv_tcp_t> tcpSocket = std::move(connect.socket_);
  co_await closeHandle(tcpSocket.get());
  BOOST_ASSERT(maybeError.has_value());
  throw std::move(maybeError.value());
}

void TcpClient::onConnect(uv_connect_t *req, uv_status status) {
  auto *connect = getRequestDataOrNull<ConnectAwaiter_>(req);
  if (connect == nullptr) {
    // Cancelled
    delete req;
    return;
  }
  if (status == UV_ECANCELED) {
    return;
  }
  connect->onConnect(status);
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
