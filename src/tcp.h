// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include <boost/assert.hpp>
#include <fmt/format.h>

#include "exception.h"
#include "internal/internal_utils.h"
#include "name_resolution.h"
#include "promise/multipromise.h"
#include "promise/promise.h"
#include "run.h"
#include "stream.h"

#include <coroutine>
#include <optional>
#include <string>
#include <utility>

namespace uvco {

/// @ingroup TCP
/// @ingroup Stream
/// A stream referring to a TCP connection.
class TcpStream : public StreamBase {
public:
  explicit TcpStream(std::unique_ptr<uv_tcp_t> tcp)
      : StreamBase{std::move(tcp)} {}
  TcpStream(const TcpStream &) = delete;
  TcpStream(TcpStream &&) = default;
  TcpStream &operator=(const TcpStream &) = delete;
  TcpStream &operator=(TcpStream &&) = default;

  ~TcpStream() override = default;

  /// Return address of peer.
  [[nodiscard]] AddressHandle getPeerName() const;

  /// Return bound address of socket.
  [[nodiscard]] AddressHandle getSockName() const;

  /// Sends RST to TCP peer and closes stream, frees associated memory.
  /// Must be awaited to avoid resource leaks.
  [[nodiscard]] Promise<void> closeReset();

  /// Set keep-alive delay in seconds.
  void keepAlive(bool enable, unsigned int delay = 10);

  /// Enable Nagle's algorithm.
  void noDelay(bool enable);
};

/// @addtogroup TCP
/// @{

/// A client for connecting to a TCP peer.
class TcpClient {
public:
  /// Create a client; call `connect()` to obtain a `TcpStream`. Address can be
  /// given as domain name, IP, etc.
  TcpClient(const Loop &loop, std::string target_host_address,
            uint16_t target_host_port, int af_hint = AF_UNSPEC);
  /// Create a TCP client connecting to the given address.
  TcpClient(const Loop &loop, AddressHandle address);

  TcpClient(TcpClient &&other) noexcept;
  TcpClient(const TcpClient &) = delete;
  TcpClient &operator=(TcpClient &&other) noexcept;
  TcpClient &operator=(const TcpClient &) = delete;
  ~TcpClient() = default;

  /// Connect to the peer specified in the constructor. The `TcpClient` object
  /// is meaningless after this call and can be destroyed.
  Promise<TcpStream> connect();

private:
  uv_loop_t *loop_;

  // May be a name or address; resolved upon connect().
  std::string host_;
  int af_hint_;
  uint16_t port_;

  static void onConnect(uv_connect_t *req, uv_status status);

  struct ConnectAwaiter_ {
    [[nodiscard]] bool await_ready() const;
    bool await_suspend(std::coroutine_handle<> h);
    int await_resume();

    void onConnect(uv_status status);

    std::optional<std::coroutine_handle<>> handle_ = {};
    std::optional<int> status_ = {};
  };
};

/// A TCP server accepts client connections by listening on a specific bind
/// address.
class TcpServer {
public:
  /// Sets up and bind socket to address.
  TcpServer(const Loop &loop, AddressHandle bindAddress, bool ipv6Only = false);

  TcpServer(const TcpServer &) = delete;
  TcpServer(TcpServer &&) = default;
  TcpServer &operator=(const TcpServer &) = delete;
  TcpServer &operator=(TcpServer &&) = default;
  ~TcpServer() = default;

  /// Return client connections as clients connect.
  ///
  /// Libuv does not appear to offer a way to stop listening and accepting
  /// connections: so we won't either.
  MultiPromise<TcpStream> listen(int backlog = 128);

  /// Close server and stop accepting client connections; must be awaited.
  Promise<void> close();

private:
  void bind(const struct sockaddr *addr, int flags);

  static void onNewConnection(uv_stream_t *stream, uv_status status);

  uv_loop_t *loop_;
  std::unique_ptr<uv_tcp_t> tcp_;

  struct ConnectionAwaiter_ {
    explicit ConnectionAwaiter_(uv_loop_t *loop) : loop_{loop} {}
    bool await_ready();
    bool await_suspend(std::coroutine_handle<> handle);
    std::optional<TcpStream> await_resume();

    void stop();

    uv_loop_t *loop_;
    std::optional<std::coroutine_handle<>> handle_;
    std::optional<TcpStream> slot_;
    std::optional<int> status_;
    bool stopped_ = false;
  };
};

/// @}

} // namespace uvco
