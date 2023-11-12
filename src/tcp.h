#pragma once

#include <uv.h>

#include "stream.h"

namespace uvco {

class TcpClient {
public:
  explicit TcpClient(uv_loop_t *loop, std::string target_host_address,
                     uint16_t target_host_port, int af_hint = AF_UNSPEC);

  TcpClient(TcpClient &&other);
  TcpClient(const TcpClient &) = delete;
  TcpClient &operator=(TcpClient &&other);
  TcpClient &operator=(const TcpClient &) = delete;
  ~TcpClient();

  Promise<void> connect();

  std::optional<Stream> &stream();

  Promise<void> close();

private:
  enum class State_ {
    initialized = 0,
    resolving = 1,
    connecting = 2,
    connected = 3,
    failed = 4,
    closing = 5,
    closed = 6,

    invalid = 7,
  };

  uv_loop_t *loop_;

  std::string host_;
  int af_hint_;
  State_ state_;
  uint16_t port_;

  // Maybe need call to uv_tcp_close_reset?
  std::optional<Stream> connected_stream_;

  static void uv_tcp_close_reset_void(uv_handle_t *handle, uv_close_cb cb);

  static void onConnect(uv_connect_t *req, int status);

  struct ConnectAwaiter_ {
    [[nodiscard]] bool await_ready() const;
    bool await_suspend(std::coroutine_handle<> h);
    int await_resume();

    void onConnect(int status);

    std::optional<std::coroutine_handle<>> handle_ = {};
    std::optional<int> status_ = {};
  };
};

class TcpServer {

  // TODO...
public:
  // Takes ownership of tcp.
  explicit TcpServer(uv_tcp_t *tcp) : tcp_{tcp} {}
  explicit TcpServer(uv_loop_t *loop);

  static constexpr const int IPV6_ONLY = UV_TCP_IPV6ONLY;

  void bind(struct sockaddr_in *addr, int flags = 0);
  void bind(struct sockaddr_in6 *addr, int flags = 0);

private:
  void bind(struct sockaddr *addr, int flags);

  std::unique_ptr<uv_tcp_t> tcp_;
};

} // namespace uvco
