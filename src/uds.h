// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "internal/internal_utils.h"
#include "promise/multipromise.h"
#include "stream.h"
#include <memory>
#include <uv.h>
namespace uvco {

/// @addtogroup Unix Sockets
/// @{

class UnixStreamServer {
  class ConnectionAwaiter_;

public:
  /// @brief Construct and bind a Unix SOCK_STREAM socket.
  /// @param loop The loop to run on.
  /// @param bindPath The path to bind to.
  /// @param flags Flags to pass to uv_pipe_bind2. Can be `UV_PIPE_NO_TRUNCATE`.
  UnixStreamServer(uv_loop_t *loop, std::string_view bindPath, int flags = 0) {
    uv_pipe_init(loop, pipe_.get(), 0);
    uv_pipe_bind2(pipe_.get(), bindPath.data(), bindPath.size(), flags);
  }

  MultiPromise<StreamBase> listen() {}

  Promise<void> close() {
    auto *listenAwaiter = (ConnectionAwaiter_ *)pipe_->data;
  }

private:
  std::unique_ptr<uv_pipe_t, UvHandleDeleter> pipe_;

  class ConnectionAwaiter_ {
  public:
    bool await_ready() { return false; }
    bool await_suspend(std::coroutine_handle<> awaitingCoroutine) {
      return true;
    }
  };
};

} // namespace uvco
