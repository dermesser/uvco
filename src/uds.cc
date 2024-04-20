// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <fmt/core.h>
#include <uv.h>
#include <uv/version.h>

#include "close.h"
#include "exception.h"
#include "internal/internal_utils.h"
#include "loop/loop.h"
#include "promise/promise.h"
#include "run.h"
#include "stream_server_base_impl.h"
#include "uds.h"
#include "uds_stream.h"

#include <coroutine>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace uvco {

UnixStreamServer::UnixStreamServer(const Loop &loop, std::string_view bindPath,
                                   int flags)
    : StreamServerBase{std::make_unique<uv_pipe_t>()} {
  uv_pipe_init(loop.uvloop(), socket_.get(), 0);
#if UV_VERSION_MAJOR == 1 && UV_VERSION_MINOR >= 46
  const uv_status bindStatus =
      uv_pipe_bind2(socket_.get(), bindPath.data(), bindPath.size(), flags);
#else
  const uv_status bindStatus = uv_pipe_bind(socket_.get(), bindPath.data());
#endif
  if (bindStatus != 0) {
    throw UvcoException{bindStatus, "UnixStreamServer failed to bind"};
  }
}

void UnixStreamServer::chmod(int mode) { uv_pipe_chmod(socket_.get(), mode); }

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
