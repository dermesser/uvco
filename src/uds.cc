// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <cstddef>
#include <string>
#include <uv.h>
#include <uv/version.h>

#include "close.h"
#include "exception.h"
#include "internal/internal_utils.h"
#include "loop/loop.h"
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
#if UV_VERSION_MAJOR == 1 && UV_VERSION_MINOR >= 46
  const uv_status bindStatus =
      uv_pipe_bind2(pipe_.get(), bindPath.data(), bindPath.size(), flags);
#else
  const uv_status bindStatus = uv_pipe_bind(pipe_.get(), bindPath.data());
#endif
  if (bindStatus != 0) {
    throw UvcoException{bindStatus, "UnixStreamServer failed to bind"};
  }
}

MultiPromise<UnixStream> UnixStreamServer::listen(int backlog) {
  ZeroAtExit<void> zeroAtExit{&pipe_->data};
  ConnectionAwaiter_ connectionAwaiter;
  pipe_->data = &connectionAwaiter;

  const uv_status listenStatus =
      uv_listen((uv_stream_t *)pipe_.get(), backlog, onNewConnection);
  if (listenStatus != 0) {
    pipe_->data = nullptr;
    throw UvcoException{listenStatus, "UnixStreamServer failed to listen"};
  }

  while (true) {
    bool ok = co_await connectionAwaiter;
    if (!ok) {
      // At this point, do not touch pipe_->data anymore!
      // This is the result of ConnectionAwaiter_::stop(), and
      // data points to a CloseAwaiter_ object.
      break;
    }

    for (auto it = connectionAwaiter.accepted_.begin();
         it != connectionAwaiter.accepted_.end(); it++) {
      auto &streamSlot = *it;

      if (streamSlot.index() == 0) {
        const uv_status status = std::get<0>(streamSlot);
        BOOST_ASSERT(status != 0);
        // When the error is handled, the user code can again call listen()
        // and will process the remaining connections. Therefore, first remove
        // the already processed connections.
        connectionAwaiter.accepted_.erase(connectionAwaiter.accepted_.begin(),
                                          it);
        throw UvcoException{status,
                            "UnixStreamServer failed to accept a connection!"};
      } else {
        co_yield std::move(std::get<1>(streamSlot));
      }
    }
    connectionAwaiter.accepted_.clear();
  }
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

  if (status == 0) {
    auto newStream = std::make_unique<uv_pipe_t>();
    uv_pipe_init(server->loop, newStream.get(), 0);
    const uv_status acceptStatus =
        uv_accept(server, (uv_stream_t *)newStream.get());
    if (acceptStatus != 0) {
      connectionAwaiter->accepted_.emplace_back(acceptStatus);
      return;
    }
    connectionAwaiter->accepted_.emplace_back(UnixStream{std::move(newStream)});
  } else {
    connectionAwaiter->accepted_.emplace_back(status);
  }

  // Resume listener coroutine.
  if (connectionAwaiter->handle_) {
    Loop::enqueue(connectionAwaiter->handle_.value());
    connectionAwaiter->handle_.reset();
  }
}

bool UnixStreamServer::ConnectionAwaiter_::await_ready() const {
  return !accepted_.empty();
}

bool UnixStreamServer::ConnectionAwaiter_::await_suspend(
    std::coroutine_handle<> awaitingCoroutine) {
  BOOST_ASSERT(!handle_);
  handle_ = awaitingCoroutine;
  return true;
}

bool UnixStreamServer::ConnectionAwaiter_::await_resume() const {
  // Stopped or no callback received.
  return !stopped_;
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

Promise<UnixStream> UnixStreamClient::connect(std::string_view path) {
  ConnectAwaiter_ awaiter{loop_, path};
  std::optional<UvcoException> maybeError;
  // Special error handling mechanism to properly close the
  // open but not connected handle.
  try {
    UnixStream connection = co_await awaiter;
    co_return std::move(connection);
  } catch (const UvcoException &e) {
    maybeError = e;
  }
  co_await closeHandle(awaiter.pipe_.get());
  BOOST_ASSERT(maybeError);
  throw std::move(maybeError).value();
}

UnixStreamClient::ConnectAwaiter_::ConnectAwaiter_(const Loop &loop,
                                                   std::string_view path)
    : pipe_{std::make_unique<uv_pipe_t>()}, path_{path} {
  uv_pipe_init(loop.uvloop(), pipe_.get(), 0);
}

void UnixStreamClient::ConnectAwaiter_::onConnect(uv_connect_t *req,
                                                  uv_status status) {
  auto *awaiter = (ConnectAwaiter_ *)req->data;
  awaiter->status_ = status;
  if (awaiter->handle_) {
    Loop::enqueue(awaiter->handle_.value());
    awaiter->handle_.reset();
  }
}

bool UnixStreamClient::ConnectAwaiter_::await_ready() { return false; }

bool UnixStreamClient::ConnectAwaiter_::await_suspend(
    std::coroutine_handle<> handle) {
  handle_ = handle;
  request_.data = this;

#if UV_VERSION_MAJOR == 1 && UV_VERSION_MINOR >= 46
  const uv_status connectStatus = uv_pipe_connect2(
      &request_, pipe_.get(), path_.data(), path_.size(), 0, onConnect);
  if (connectStatus != 0) {
    status_ = connectStatus;
    if (handle_) {
      Loop::enqueue(*handle_);
      handle_.reset();
    }
  }
#else
  uv_pipe_connect(&request_, pipe_.get(), path_.data(), onConnect);
#endif

  return true;
}

UnixStream UnixStreamClient::ConnectAwaiter_::await_resume() {
  BOOST_ASSERT(status_);
  if (*status_ != 0) {
    throw UvcoException{*status_, "UnixStreamClient failed to connect"};
  }
  status_.reset();
  handle_.reset();
  return UnixStream{std::move(pipe_)};
}

} // namespace uvco
