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
  UnixStreamServer(const Loop &loop, std::string_view bindPath, int flags = 0)
      : pipe_{std::make_unique<uv_pipe_t>()} {
    uv_pipe_init(loop.uvloop(), pipe_.get(), 0);
    const uv_status bindStatus =
        uv_pipe_bind2(pipe_.get(), bindPath.data(), bindPath.size(), flags);
    if (bindStatus != 0) {
      throw UvcoException{bindStatus, "UnixStreamServer failed to bind"};
    }
  }

  MultiPromise<StreamBase> listen(unsigned int backlog = 128) {
    ConnectionAwaiter_ connectionAwaiter;
    pipe_->data = &connectionAwaiter;

    uv_listen((uv_stream_t *)pipe_.get(), backlog, onNewConnection);
    while (true) {
      std::optional<StreamBase> stream = co_await connectionAwaiter;
      if (!stream) {
        break;
      }
      co_yield std::move(*stream);
    }
    pipe_->data = nullptr;
  }

  Promise<void> close() {
    ConnectionAwaiter_ *connectionAwaiter = (ConnectionAwaiter_ *)pipe_->data;
    if (connectionAwaiter != nullptr && connectionAwaiter->handle_) {
      connectionAwaiter->stop();
    }
    co_await closeHandle(pipe_.get());
  }

private:
  std::unique_ptr<uv_pipe_t> pipe_;

  static void onNewConnection(uv_stream_t *server, uv_status status) {
    BOOST_ASSERT(server->type == UV_NAMED_PIPE);
    auto *connectionAwaiter = (ConnectionAwaiter_ *)server->data;

    connectionAwaiter->status_ = status;
    if (status == 0) {
      BOOST_ASSERT(!connectionAwaiter->streamSlot_);
      auto newStream = std::make_unique<uv_pipe_t>();
      uv_pipe_init(server->loop, newStream.get(), 0);
      const uv_status acceptStatus =
          uv_accept(server, (uv_stream_t *)newStream.get());
      if (acceptStatus != 0) {
        BOOST_ASSERT(!connectionAwaiter->status_);
        connectionAwaiter->status_ = acceptStatus;
        return;
      }
      connectionAwaiter->streamSlot_ = StreamBase{std::move(newStream)};
    }

    // Resume listener coroutine.
    if (connectionAwaiter->handle_) {
      Loop::enqueue(connectionAwaiter->handle_.value());
      connectionAwaiter->handle_.reset();
    }
  }

  /// Used to track a pending listener, waiting for a connection.
  struct ConnectionAwaiter_ {
    bool await_ready() { return streamSlot_.has_value(); }

    bool await_suspend(std::coroutine_handle<> awaitingCoroutine) {
      fmt::print("await_suspend\n");
      BOOST_ASSERT(!handle_);
      BOOST_ASSERT(!streamSlot_);
      BOOST_ASSERT(!status_);
      handle_ = awaitingCoroutine;
      return true;
    }

    std::optional<StreamBase> await_resume() {
      fmt::print("await_resume\n");
      // Stopped or no callback received.
      if (stopped_ || !status_) {
        return std::nullopt;
      }

      if (*status_ == 0) {
        BOOST_ASSERT(streamSlot_);
        std::optional<StreamBase> result = std::move(streamSlot_);
        streamSlot_.reset();
        status_.reset();
        return result;
      } else {
        BOOST_ASSERT(!streamSlot_);
        const uv_status status = *status_;
        status_.reset();
        throw UvcoException{status,
                            "UnixStreamServer received error while listening"};
      }
    }

    void stop() {
      if (stopped_) {
        return;
      }
      stopped_ = true;
      if (handle_) {
        Loop::enqueue(handle_.value());
        handle_.reset();
      }
    }

    std::optional<std::coroutine_handle<>> handle_;
    std::optional<StreamBase> streamSlot_;
    std::optional<uv_status> status_;
    bool stopped_ = false;
  };
};

} // namespace uvco
