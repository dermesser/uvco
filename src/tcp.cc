// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include "tcp.h"

#include "name_resolution.h"
#include "stream.h"

namespace uvco {

TcpClient::TcpClient(uv_loop_t *loop, std::string target_host_address,
                     uint16_t target_host_port, int af_hint)
    : loop_{loop}, host_{std::move(target_host_address)}, af_hint_{af_hint},
      port_{target_host_port} {}

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
  Resolver resolver{loop_};

  AddressHandle ah =
      co_await resolver.gai(host_, fmt::format("{}", port_), af_hint_);

  uv_connect_t req;
  ConnectAwaiter_ connect{};
  req.data = &connect;

  auto tcp = std::make_unique<uv_tcp_t>();

  uv_tcp_init(loop_, tcp.get());
  uv_tcp_connect(&req, tcp.get(), ah.sockaddr(), onConnect);

  int status = co_await connect;
  if (status < 0) {
    throw UvcoException(status, "connect");
  }

  co_return TcpStream{tcp.release()};
}

void TcpClient::onConnect(uv_connect_t *req, int status) {
  auto *connect = static_cast<ConnectAwaiter_ *>(req->data);
  connect->onConnect(status);
}

bool TcpClient::ConnectAwaiter_::await_ready() const {
  return status_.has_value();
}

bool TcpClient::ConnectAwaiter_::await_suspend(std::coroutine_handle<> handle) {
  assert(!handle_);
  handle_ = handle;
  return true;
}

int TcpClient::ConnectAwaiter_::await_resume() {
  assert(status_);
  return *status_;
}

void TcpClient::ConnectAwaiter_::onConnect(int status) {
  status_ = status;
  if (handle_) {
    handle_->resume();
  }
}

TcpServer::TcpServer(uv_loop_t *loop, AddressHandle bindAddress, bool ipv6Only)
    : loop_{loop}, tcp_{} {
  uv_tcp_init(loop, &tcp_);
  const auto *sa = bindAddress.sockaddr();
  const unsigned flags = ipv6Only ? UV_TCP_IPV6ONLY : 0;
  bind(sa, flags);
}

void TcpServer::bind(const struct sockaddr *addr, int flags) {
  int result = uv_tcp_bind(&tcp_, addr, flags);
  if (result != 0) {
    throw UvcoException{result, "TcpServer::bind()"};
  }
}

MultiPromise<StreamBase> TcpServer::listen(int backlog) {
  ConnectionAwaiter_ awaiter{loop_};
  tcp_.data = &awaiter;

  uv_listen((uv_stream_t *)&tcp_, backlog, onNewConnection);

  while (true) {
    // TODO: specialize stream to enable getsockname etc.
    std::optional<TcpStream> stream = co_await awaiter;
    if (!stream)
      break;
    co_yield std::move(*stream);
  }
}

void TcpServer::onNewConnection(uv_stream_t *stream, int status) {
  const auto *server = (uv_tcp_t *)stream;
  auto *awaiter = (ConnectionAwaiter_ *)server->data;
  uv_loop_t *const loop = awaiter->loop_;

  awaiter->status_ = status;
  if (status == 0) {
    auto *const clientStream = new uv_tcp_t{};
    uv_tcp_init(loop, clientStream);
    uv_accept((uv_stream_t *)server, (uv_stream_t *)clientStream);
    assert(!awaiter->slot_);
    awaiter->slot_ = TcpStream{clientStream};
  }
  awaiter->handle_->resume();
}

} // namespace uvco
