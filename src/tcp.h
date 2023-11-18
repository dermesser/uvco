// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>
#include <boost/assert.hpp>

#include "close.h"
#include "name_resolution.h"
#include "stream.h"

namespace uvco {

class TcpStream : public StreamBase {
public:
  // Takes ownership of tcp. tcp must be dynamically allocated.
  explicit TcpStream(uv_tcp_t *tcp) : StreamBase{(uv_stream_t *)tcp} {}
  TcpStream(const TcpStream &) = delete;
  TcpStream(TcpStream &&) = default;
  TcpStream &operator=(const TcpStream &) = delete;
  TcpStream &operator=(TcpStream &&) = default;

  ~TcpStream() override = default;

  AddressHandle getPeerName() const {
    struct sockaddr_storage addr {};
    int namelen = sizeof(addr);
    uv_tcp_getpeername((const uv_tcp_t *)underlying(), (struct sockaddr *)&addr,
                       &namelen);
    const AddressHandle address{(struct sockaddr *)&addr};
    return address;
  };

  AddressHandle getSockName() const {
    struct sockaddr_storage addr {};
    int namelen = sizeof(addr);
    uv_tcp_getsockname((const uv_tcp_t *)underlying(), (struct sockaddr *)&addr,
                       &namelen);
    const AddressHandle address{(struct sockaddr *)&addr};
    return address;
  }

  // Sends RST to TCP peer.
  // Must be awaited.
  [[nodiscard]] Promise<void> closeReset() {
    CloseAwaiter awaiter{};
    stream().data = &awaiter;
    uv_tcp_close_reset((uv_tcp_t *)&stream(), onCloseCallback);
    co_await awaiter;
    destroyStream();
  }

  void keepAlive(bool enable, unsigned int delay = 10) {
    uv_tcp_keepalive((uv_tcp_t *)&stream(), static_cast<int>(enable), delay);
  }

  void noDelay(bool enable) {
    uv_tcp_nodelay((uv_tcp_t *)&stream(), static_cast<int>(enable));
  }
};

class TcpClient {
public:
  explicit TcpClient(uv_loop_t *loop, std::string target_host_address,
                     uint16_t target_host_port, int af_hint = AF_UNSPEC);

  TcpClient(TcpClient &&other) noexcept;
  TcpClient(const TcpClient &) = delete;
  TcpClient &operator=(TcpClient &&other) noexcept;
  TcpClient &operator=(const TcpClient &) = delete;
  ~TcpClient() = default;

  Promise<TcpStream> connect();

private:
  uv_loop_t *loop_;

  std::string host_;
  int af_hint_;
  uint16_t port_;

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
  ~TcpServer() = default;

  // Libuv does not appear to offer a way to stop listening and accepting
  // connections: so we won't either.
  MultiPromise<TcpStream> listen(int backlog = 128);

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
    std::optional<TcpStream> await_resume() {
      if (stopped_)
        return {};
      BOOST_ASSERT(status_);

      if (*status_ == 0) {
        BOOST_ASSERT(slot_);
        TcpStream stream{std::move(*slot_)};
        status_.reset();
        slot_.reset();
        return stream;
      } else {
        int status = *status_;
        BOOST_ASSERT(!slot_);
        status_.reset();
        throw UvcoException(status, "TcpServer::listen()");
      }
    }

    void stop() {
      stopped_ = true;
      if (handle_) {
        handle_->resume();
      }
    }

    uv_loop_t *loop_;
    std::optional<std::coroutine_handle<>> handle_;
    std::optional<TcpStream> slot_;
    std::optional<int> status_;
    bool stopped_ = false;
  };
};

} // namespace uvco
