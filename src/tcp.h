// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <boost/assert.hpp>
#include <uv.h>

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

  [[nodiscard]] AddressHandle getPeerName() const;

  [[nodiscard]] AddressHandle getSockName() const;

  // Sends RST to TCP peer.
  // Must be awaited.
  [[nodiscard]] Promise<void> closeReset();

  void keepAlive(bool enable, unsigned int delay = 10);

  void noDelay(bool enable);
};

class TcpClient {
public:
  // Create a client; call connect() to obtain a TcpStream. Address can be given
  // as domain name, IP, etc.
  TcpClient(uv_loop_t *loop, std::string target_host_address,
            uint16_t target_host_port, int af_hint = AF_UNSPEC);
  TcpClient(uv_loop_t *loop, AddressHandle address);

  TcpClient(TcpClient &&other) noexcept;
  TcpClient(const TcpClient &) = delete;
  TcpClient &operator=(TcpClient &&other) noexcept;
  TcpClient &operator=(const TcpClient &) = delete;
  ~TcpClient() = default;

  // TODO: maybe replace with static connect function.
  Promise<TcpStream> connect();

private:
  uv_loop_t *loop_;

  // May be a name or address; resolved upon connect().
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

  Promise<void> close();

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
