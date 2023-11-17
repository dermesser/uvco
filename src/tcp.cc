// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include "tcp.h"

#include "name_resolution.h"
#include "stream.h"

namespace uvco {

TcpClient::TcpClient(uv_loop_t *loop, std::string target_host_address,
                     uint16_t target_host_port, int af_hint)
    : loop_{loop}, host_{std::move(target_host_address)}, af_hint_{af_hint},
      state_{State_::initialized}, port_{target_host_port} {}
TcpClient::TcpClient(TcpClient &&other)
    : loop_{other.loop_}, host_{std::move(other.host_)},
      af_hint_{other.af_hint_}, state_{other.state_}, port_{other.port_} {
  other.state_ = State_::invalid;
}

TcpClient &TcpClient::operator=(TcpClient &&other) {
  loop_ = other.loop_;
  host_ = std::move(other.host_);
  port_ = other.port_;
  state_ = other.state_;
  other.state_ = State_::invalid;
  return *this;
}

TcpClient::~TcpClient() { assert(state_ != State_::connected); }

Promise<void> TcpClient::connect() {
  Resolver resolver{loop_};

  state_ = State_::resolving;

  AddressHandle ah =
      co_await resolver.gai(host_, fmt::format("{}", port_), af_hint_);
  fmt::print("Resolution OK: {}\n", ah.toString());

  uv_connect_t req;
  ConnectAwaiter_ connect{};
  req.data = &connect;

  state_ = State_::connecting;

  auto tcp = std::make_unique<uv_tcp_t>();

  uv_tcp_init(loop_, tcp.get());
  uv_tcp_connect(&req, tcp.get(), ah.sockaddr(), onConnect);

  co_await connect;

  if (connect.status_ < 0) {
    throw UvcoException(*connect.status_, "connect");
  }
  state_ = State_::connected;
  fmt::print("Connected successfully to {}:{}\n", host_, port_);
  connected_stream_ = Stream{(uv_stream_t *)(tcp.release())};
}

std::optional<Stream> &TcpClient::stream() { return connected_stream_; }
Promise<void> TcpClient::close() {
  assert(connected_stream_);
  if (connected_stream_) {
    state_ = State_::closing;
    co_await connected_stream_->close(uv_tcp_close_reset_void);
    state_ = State_::closed;
    connected_stream_.reset();
  }
  co_return;
}

void TcpClient::uv_tcp_close_reset_void(uv_handle_t *handle, uv_close_cb cb) {
  uv_tcp_close_reset((uv_tcp_t *)handle, cb);
}

void TcpClient::onConnect(uv_connect_t *req, int status) {
  fmt::print("onConnect from UV\n");
  auto *connect = static_cast<ConnectAwaiter_ *>(req->data);
  connect->onConnect(status);
}

bool TcpClient::ConnectAwaiter_::await_ready() const {
  return status_.has_value();
}

bool TcpClient::ConnectAwaiter_::await_suspend(std::coroutine_handle<> h) {
  assert(!handle_);
  handle_ = h;
  return true;
}

int TcpClient::ConnectAwaiter_::await_resume() {
  assert(status_);
  return *status_;
}

void TcpClient::ConnectAwaiter_::onConnect(int status) {
  status_ = status;
  if (handle_)
    handle_->resume();
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

MultiPromise<Stream> TcpServer::listen(int backlog) {
  ConnectionAwaiter_ awaiter{loop_};
  tcp_.data = &awaiter;

  uv_listen((uv_stream_t *)&tcp_, backlog, onNewConnection);

  // TODO: elegant way to shut down listener loop?
  while (true) {
    // TODO: specialize stream to enable getsockname etc.
    Stream stream = co_await awaiter;
    co_yield std::move(stream);
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
    awaiter->slot_ = Stream{(uv_stream_t *)clientStream};
  }
  awaiter->handle_->resume();
}

} // namespace uvco
