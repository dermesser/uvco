// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <uv.h>

#include "close.h"
#include "exception.h"
#include "internal/internal_utils.h"
#include "name_resolution.h"
#include "promise/multipromise.h"
#include "promise/promise.h"
#include "run.h"
#include "tcp.h"

#include <boost/assert.hpp>
#include <coroutine>
#include <cstdint>
#include <fmt/core.h>
#include <fmt/format.h>
#include <memory>
#include <optional>
#include <string>
#include <sys/socket.h>
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
  uv_status status =
      uv_tcp_connect(&req, tcp.get(), address.sockaddr(), onConnect);
  if (status < 0) {
    // Clean up handle if connect failed.
    co_await closeHandle(tcp.get());
    throw UvcoException(status, "TcpClient::connect() failed immediately");
  }

  status = co_await connect;
  if (status < 0) {
    co_await closeHandle(tcp.get());
    throw UvcoException(status, "TcpClient::connect() failed");
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

int TcpClient::ConnectAwaiter_::await_resume() {
  BOOST_ASSERT(status_);
  return *status_;
}

void TcpClient::ConnectAwaiter_::onConnect(uv_status status) {
  status_ = status;
  if (handle_) {
    handle_->resume();
  }
}

TcpServer::TcpServer(const Loop &loop, AddressHandle bindAddress, bool ipv6Only)
    : loop_{loop.uvloop()}, tcp_{std::make_unique<uv_tcp_t>()} {
  uv_tcp_init(loop_, tcp_.get());
  const auto *sockaddr = bindAddress.sockaddr();
  const int flags = ipv6Only ? UV_TCP_IPV6ONLY : 0;
  bind(sockaddr, flags);
}

void TcpServer::bind(const struct sockaddr *addr, int flags) {
  int result = uv_tcp_bind(tcp_.get(), addr, flags);
  if (result != 0) {
    throw UvcoException{result, "TcpServer::bind()"};
  }
}

MultiPromise<TcpStream> TcpServer::listen(int backlog) {
  ConnectionAwaiter_ awaiter{loop_};
  tcp_->data = &awaiter;

  uv_listen((uv_stream_t *)tcp_.get(), backlog, onNewConnection);

  while (true) {
    // TODO: specialize stream to enable getsockname etc.
    std::optional<TcpStream> stream = co_await awaiter;
    if (!stream) {
      break;
    }
    co_yield std::move(*stream);
  }
}

Promise<void> TcpServer::close() {
  auto *awaiter = (ConnectionAwaiter_ *)tcp_->data;
  // Resume listener coroutine.
  if (awaiter != nullptr && awaiter->handle_) {
    awaiter->stop();
  }
  co_await closeHandle(tcp_.get());
}

void TcpServer::onNewConnection(uv_stream_t *stream, uv_status status) {
  const auto *server = (uv_tcp_t *)stream;
  auto *awaiter = (ConnectionAwaiter_ *)server->data;
  uv_loop_t *const loop = awaiter->loop_;

  awaiter->status_ = status;
  if (status == 0) {
    auto clientStream = std::make_unique<uv_tcp_t>();
    uv_tcp_init(loop, clientStream.get());
    uv_accept((uv_stream_t *)server, (uv_stream_t *)clientStream.get());
    BOOST_ASSERT(!awaiter->slot_);
    awaiter->slot_ = TcpStream{std::move(clientStream)};
  }
  awaiter->handle_->resume();
}

Promise<void> TcpStream::closeReset() {
  co_await closeHandle((uv_tcp_t *)&stream(), uv_tcp_close_reset);
  destroyStream();
}

AddressHandle TcpStream::getSockName() const {
  struct sockaddr_storage addr {};
  int namelen = sizeof(addr);
  uv_tcp_getsockname((const uv_tcp_t *)underlying(), (struct sockaddr *)&addr,
                     &namelen);
  const AddressHandle address{(struct sockaddr *)&addr};
  return address;
}

AddressHandle TcpStream::getPeerName() const {
  struct sockaddr_storage addr {};
  int namelen = sizeof(addr);
  uv_tcp_getpeername((const uv_tcp_t *)underlying(), (struct sockaddr *)&addr,
                     &namelen);
  const AddressHandle address{(struct sockaddr *)&addr};
  return address;
};

void TcpStream::keepAlive(bool enable, unsigned int delay) {
  uv_tcp_keepalive((uv_tcp_t *)&stream(), static_cast<int>(enable), delay);
}

void TcpStream::noDelay(bool enable) {
  uv_tcp_nodelay((uv_tcp_t *)&stream(), static_cast<int>(enable));
}

std::optional<TcpStream> TcpServer::ConnectionAwaiter_::await_resume() {
  // !status_ if close() resumed us.
  if (stopped_) {
    return {};
  }
  if (!status_) {
    return {};
  }

  BOOST_ASSERT(status_);

  if (*status_ == 0) {
    BOOST_ASSERT(slot_);
    TcpStream stream{std::move(*slot_)};
    status_.reset();
    slot_.reset();
    return stream;
  } else {
    uv_status status = *status_;
    BOOST_ASSERT(!slot_);
    status_.reset();
    throw UvcoException(status, "TcpServer::listen()");
  }
}

void TcpServer::ConnectionAwaiter_::stop() {
  stopped_ = true;
  if (handle_) {
    handle_->resume();
  }
}

bool TcpServer::ConnectionAwaiter_::await_suspend(
    std::coroutine_handle<> handle) {
  handle_ = handle;
  return true;
}

bool TcpServer::ConnectionAwaiter_::await_ready() { return false; }

} // namespace uvco
