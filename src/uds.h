// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <cstddef>
#include <fmt/core.h>
#include <string>
#include <uv.h>

#include <coroutine>
#include <memory>
#include <optional>
#include <string_view>

#include "exception.h"
#include "internal/internal_utils.h"
#include "promise/multipromise.h"
#include "promise/promise.h"
#include "run.h"
#include "stream.h"

namespace uvco {

/// @addtogroup Unix Sockets
/// @{

/// A stream served by a Unix domain socket. In addition to the `StreamBase`
/// functionality, it provides getSockname() and getPeerName() methods.
class UnixStream : public StreamBase {
public:
  using StreamBase::StreamBase;

  std::string getSockName();
  std::string getPeerName();
};

class UnixStreamServer {
  class ConnectionAwaiter_;

public:
  /// @brief Construct and bind a Unix SOCK_STREAM socket.
  /// @param loop The loop to run on.
  /// @param bindPath The path to bind to.
  /// @param flags Flags to pass to uv_pipe_bind2. Can be `UV_PIPE_NO_TRUNCATE`.
  UnixStreamServer(const Loop &loop, std::string_view bindPath, int flags = 0);

  /// Configure permissions for socket; flags may be either or a combination of
  /// `UV_READABLE` and `UV_WRITABLE`.
  void chmod(int mode);

  /// Listen on the given socket. Yields a stream for each incoming connection.
  MultiPromise<UnixStream> listen(int backlog = 128);

  /// Close the server. Ensure to await this to avoid resource leaks.
  Promise<void> close();

private:
  std::unique_ptr<uv_pipe_t> pipe_;

  static void onNewConnection(uv_stream_t *server, uv_status status);

  /// Used to track a pending listener, waiting for a connection.
  struct ConnectionAwaiter_ {
    [[nodiscard]] bool await_ready() const;
    bool await_suspend(std::coroutine_handle<> awaitingCoroutine);
    std::optional<UnixStream> await_resume();
    void stop();

    std::optional<std::coroutine_handle<>> handle_;
    std::optional<UnixStream> streamSlot_;
    std::optional<uv_status> status_;
    bool stopped_ = false;
  };
};

/// @}

} // namespace uvco
