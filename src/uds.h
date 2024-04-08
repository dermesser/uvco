// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <fmt/core.h>
#include <uv.h>

#include <coroutine>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "close.h"
#include "exception.h"
#include "internal/internal_utils.h"
#include "promise/multipromise.h"
#include "promise/promise.h"
#include "run.h"
#include "stream.h"

namespace uvco {

/// @addtogroup Unix Sockets
/// @{

/// TODO: not yet implemented!
class UnixStreamServer {
  class ConnectionAwaiter_;

public:
  /// @brief Construct and bind a Unix SOCK_STREAM socket.
  /// @param loop The loop to run on.
  /// @param bindPath The path to bind to.
  /// @param flags Flags to pass to uv_pipe_bind2. Can be `UV_PIPE_NO_TRUNCATE`.
  UnixStreamServer(const Loop &loop, std::string_view bindPath, int flags = 0);

  /// Listen on the given socket. Yields a stream for each incoming connection.
  MultiPromise<StreamBase> listen(int backlog = 128);

  /// Close the server. Ensure to await this to avoid resource leaks.
  Promise<void> close();

private:
  std::unique_ptr<uv_pipe_t> pipe_;

  static void onNewConnection(uv_stream_t *server, uv_status status);

  /// Used to track a pending listener, waiting for a connection.
  struct ConnectionAwaiter_ {
    [[nodiscard]] bool await_ready() const;
    bool await_suspend(std::coroutine_handle<> awaitingCoroutine);
    std::optional<StreamBase> await_resume();
    void stop();

    std::optional<std::coroutine_handle<>> handle_;
    std::optional<StreamBase> streamSlot_;
    std::optional<uv_status> status_;
    bool stopped_ = false;
  };
};

} // namespace uvco
