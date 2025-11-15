// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include <boost/assert.hpp>
#include <fmt/format.h>

#include "uvco/internal/internal_utils.h"
#include "uvco/name_resolution.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"
#include "uvco/stream_server_base.h"
#include "uvco/tcp_stream.h"

#include <cstdint>
#include <string>

namespace uvco {

/// @addtogroup TCP
/// @{

/// A client for connecting to a TCP peer.
class TcpClient {
  struct ConnectAwaiter_;

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
  const Loop *loop_;

  // May be a name or address; resolved upon connect().
  std::string host_;
  int af_hint_;
  uint16_t port_;

  static void onConnect(uv_connect_t *req, uv_status status);
};

/// A TCP server accepts client connections by listening on a specific bind
/// address.
class TcpServer : public StreamServerBase<uv_tcp_t, TcpStream> {
public:
  /// Sets up and bind socket to address.
  TcpServer(const Loop &loop, AddressHandle bindAddress, bool ipv6Only = false);

  TcpServer(const TcpServer &) = delete;
  TcpServer(TcpServer &&) = default;
  TcpServer &operator=(const TcpServer &) = delete;
  TcpServer &operator=(TcpServer &&) = default;
  ~TcpServer() = default;

private:
  void bind(const struct sockaddr *addr, int flags);
};

/// @}

} // namespace uvco
