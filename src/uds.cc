// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <cstddef>
#include <fmt/core.h>
#include <string>
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

MultiPromise<UnixStream> UnixStreamServer::listen(int backlog) {
  ConnectionAwaiter_ connectionAwaiter;
  pipe_->data = &connectionAwaiter;

  uv_listen((uv_stream_t *)pipe_.get(), backlog, onNewConnection);
  while (true) {
    std::optional<UnixStream> stream = co_await connectionAwaiter;
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

void UnixStreamServer::chmod(int mode) { uv_pipe_chmod(pipe_.get(), mode); }

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
    connectionAwaiter->streamSlot_ = UnixStream{std::move(newStream)};
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
  BOOST_ASSERT(!handle_);
  BOOST_ASSERT(!streamSlot_);
  BOOST_ASSERT(!status_);
  handle_ = awaitingCoroutine;
  return true;
}

std::optional<UnixStream> UnixStreamServer::ConnectionAwaiter_::await_resume() {
  // Stopped or no callback received.
  if (stopped_ || !status_) {
    return std::nullopt;
  }

  if (*status_ == 0) {
    BOOST_ASSERT(streamSlot_);
    std::optional<UnixStream> result = std::move(streamSlot_);
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

namespace {

std::string getXname(uv_pipe_t *stream,
                     int (*getName)(const uv_pipe_t *, char *, size_t *)) {
  static constexpr size_t maxPath = 1024;
  std::string path;
  path.resize(maxPath);
  size_t pathSize = maxPath;
  const uv_status status = getName(stream, path.data(), &pathSize);
  if (status != 0) {
    throw UvcoException{status, "UnixStream::getXName failed"};
  }
  path.resize(pathSize);
  return path;
}

} // namespace

std::string UnixStream::getSockName() {
  return getXname((uv_pipe_t *)&stream(), uv_pipe_getsockname);
}

std::string UnixStream::getPeerName() {
  return getXname((uv_pipe_t *)&stream(), uv_pipe_getpeername);
}

} // namespace uvco
