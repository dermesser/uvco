// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include "name_resolution.h"
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
  // Sets up and binds socket to address.
  TcpServer(uv_loop_t *loop, AddressHandle bindAddress, bool ipv6Only = false);

  TcpServer(const TcpServer &) = delete;
  TcpServer(TcpServer &&) = default;
  TcpServer &operator=(const TcpServer &) = delete;
  TcpServer &operator=(TcpServer &&) = default;

  MultiPromise<Stream> listen(int backlog = 128);

private:
  void bind(const struct sockaddr *addr, int flags);

  static void onNewConnection(uv_stream_t *server, int status);

  uv_loop_t *loop_;
  uv_tcp_t tcp_;

  struct ConnectionAwaiter_ {
    explicit ConnectionAwaiter_(uv_loop_t *loop) : loop_{loop} {}
    bool await_ready() { return false; }
    bool await_suspend(std::coroutine_handle<> handle) {
      handle_ = handle;
      return true;
    }
    Stream await_resume() {
      assert(status_);

      if (*status_ == 0) {
        Stream stream{std::move(*slot_)};
        status_.reset();
        slot_.reset();
        return stream;
      } else {
        int status = *status_;
        assert(!slot_);
        status_.reset();
        throw UvcoException(status, "TcpServer::listen()");
      }
    }

    uv_loop_t *loop_;
    std::optional<std::coroutine_handle<>> handle_;
    std::optional<Stream> slot_;
    std::optional<int> status_;
  };
};

} // namespace uvco
