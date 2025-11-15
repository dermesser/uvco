// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <fmt/core.h>
#include <uv.h>

#include "uvco/promise/promise.h"
#include "uvco/run.h"
#include "uvco/stream_server_base.h"
#include "uvco/uds_stream.h"

#include <string_view>

namespace uvco {

/// @addtogroup UnixSockets
/// @{

/// A client that connects to a Unix domain socket (type `SOCK_STREAM`).
///
/// The `connect()` method returns a `Promise<UnixStream>`, which will resolve
/// when the connection is established. The peer address can be obtained using
/// the `getPeerName()` method on `UnixStream`.
class UnixStreamClient {
  struct ConnectAwaiter_;

public:
  UnixStreamClient(const UnixStreamClient &) = delete;
  UnixStreamClient(UnixStreamClient &&) = delete;
  UnixStreamClient &operator=(const UnixStreamClient &) = delete;
  UnixStreamClient &operator=(UnixStreamClient &&) = delete;
  ~UnixStreamClient() = default;

  explicit UnixStreamClient(const Loop &loop) : loop_{loop} {}

  /// Connect to a Unix domain socket at the given path.
  Promise<UnixStream> connect(std::string_view path);

private:
  const Loop &loop_;
};

/// A server that listens for incoming connections on a Unix domain socket (type
/// `SOCK_STREAM`).
///
/// After constructing the server, call `listen()` to start listening. This will
/// return a `MultiPromise<UnixStream>`, which will yield a new `UnixStream` for
/// each incoming connection. The peer address can be obtained using the
/// `getPeerName()` method on `UnixStream`.
class UnixStreamServer : public StreamServerBase<uv_pipe_t, UnixStream> {
public:
  UnixStreamServer(const UnixStreamServer &) = delete;
  UnixStreamServer(UnixStreamServer &&) = default;
  UnixStreamServer &operator=(const UnixStreamServer &) = delete;
  UnixStreamServer &operator=(UnixStreamServer &&) = default;
  ~UnixStreamServer() = default;

  /// @brief Construct and bind a Unix SOCK_STREAM socket.
  /// @param loop The loop to run on.
  /// @param bindPath The path to bind to.
  /// @param flags Flags to pass to uv_pipe_bind2. Can be `UV_PIPE_NO_TRUNCATE`.
  UnixStreamServer(const Loop &loop, std::string_view bindPath, int flags = 0);

  /// Configure permissions for socket; flags may be either or a combination of
  /// `UV_READABLE` and `UV_WRITABLE`.
  void chmod(int mode);
};

/// @}

} // namespace uvco
