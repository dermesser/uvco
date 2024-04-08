// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <fmt/core.h>
#include <uv.h>

#include "close.h"
#include "exception.h"
#include "internal/internal_utils.h"
#include "promise/multipromise.h"
#include "promise/promise.h"
#include "run.h"
#include "stream.h"
#include "uds.h"

#include <coroutine>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace uvco {

UnixStreamServer::UnixStreamServer(const Loop &loop, std::string_view bindPath,
                                   int flags)
    : pipe_{std::make_unique<uv_pipe_t>()} {
  uv_pipe_init(loop.uvloop(), pipe_.get(), 0);
  const uv_status bindStatus =
      uv_pipe_bind2(pipe_.get(), bindPath.data(), bindPath.size(), flags);
  if (bindStatus != 0) {
    throw UvcoException{bindStatus, "UnixStreamServer failed to bind"};
  }
}

MultiPromise<StreamBase> UnixStreamServer::listen(int backlog) {
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

Promise<void> UnixStreamServer::close() {
  auto *connectionAwaiter = (ConnectionAwaiter_ *)pipe_->data;
  if (connectionAwaiter != nullptr && connectionAwaiter->handle_) {
    connectionAwaiter->stop();
  }
  co_await closeHandle(pipe_.get());
}

void UnixStreamServer::onNewConnection(uv_stream_t *server, uv_status status) {
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

bool UnixStreamServer::ConnectionAwaiter_::await_ready() const {
  return streamSlot_.has_value();
}

bool UnixStreamServer::ConnectionAwaiter_::await_suspend(
    std::coroutine_handle<> awaitingCoroutine) {
  fmt::print("await_suspend\n");
  BOOST_ASSERT(!handle_);
  BOOST_ASSERT(!streamSlot_);
  BOOST_ASSERT(!status_);
  handle_ = awaitingCoroutine;
  return true;
}

std::optional<StreamBase> UnixStreamServer::ConnectionAwaiter_::await_resume() {
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

void UnixStreamServer::ConnectionAwaiter_::stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  if (handle_) {
    Loop::enqueue(handle_.value());
    handle_.reset();
  }
}

} // namespace uvco
