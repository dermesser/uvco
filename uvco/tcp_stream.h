// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include <boost/assert.hpp>
#include <fmt/format.h>

#include "uvco/name_resolution.h"
#include "uvco/promise/promise.h"
#include "uvco/stream.h"

#include <memory>
#include <sys/socket.h>
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

} // namespace uvco
