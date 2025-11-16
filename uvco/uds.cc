// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <fmt/core.h>
#include <uv.h>
#include <uv/version.h>

#include "uvco/close.h"
#include "uvco/exception.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"
#include "uvco/stream_server_base.h"
#include "uvco/uds.h"
#include "uvco/uds_stream.h"

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

/// An awaiter class used to wait for a connection to be established.
///
/// Implementation note: almost the entire mechanics of connecting is
/// handled by the awaiter. The `connect()` method is just a thin wrapper
/// around the awaiter; the awaiter's methods also throw exceptions. This
/// is different than e.g. in the `TcpClient` class.
struct UnixStreamClient::ConnectAwaiter_ {
  explicit ConnectAwaiter_(const Loop &loop);
  ~ConnectAwaiter_();

  static void onConnect(uv_connect_t *req, uv_status status);

  [[nodiscard]] static bool await_ready();
  bool await_suspend(std::coroutine_handle<> handle);
  UnixStream await_resume();

  std::unique_ptr<uv_connect_t> request_{};
  std::unique_ptr<uv_pipe_t> pipe_;
  std::coroutine_handle<> handle_;
  uv_status status_{};
};

UnixStreamClient::ConnectAwaiter_::ConnectAwaiter_(const Loop &loop)
    : request_{std::make_unique<uv_connect_t>()},
      pipe_{std::make_unique<uv_pipe_t>()} {
  uv_pipe_init(loop.uvloop(), pipe_.get(), 0);
  setRequestData(request_.get(), this);
}

UnixStreamClient::ConnectAwaiter_::~ConnectAwaiter_() {
  // request data is reset by await_resume(), so if it's non-null, the
  // connection attempt was cancelled and we need to clean up.
  if (pipe_ != nullptr) {
    closeHandle(pipe_.release());
  }
  if (!requestDataIsNull(request_.get())) {
    resetRequestData(request_.get());
    uv_cancel((uv_req_t *)request_.release());
  }
}

Promise<UnixStream> UnixStreamClient::connect(std::string_view path) {
  ConnectAwaiter_ connect{loop_};

#if UV_VERSION_MAJOR == 1 && UV_VERSION_MINOR >= 46
  const uv_status connectStatus =
      uv_pipe_connect2(connect.request_.get(), connect.pipe_.get(), path.data(),
                       path.size(), 0, ConnectAwaiter_::onConnect);
  if (connectStatus != 0) {
    co_await closeHandle(connect.pipe_.get());
    connect.pipe_.reset();
    throw UvcoException(connectStatus,
                        "UnixStreamClient::connect() failed immediately");
  }
#else
  uv_pipe_connect(&request_, pipe_.get(), path_.data(), onConnect);
#endif

  std::optional<UvcoException> maybeError;
  // Special error handling mechanism to properly close the
  // open but not connected handle.
  try {
    co_return (co_await connect);
  } catch (const UvcoException &e) {
    maybeError = e;
  }
  const std::unique_ptr<uv_pipe_t> pipe = std::move(connect.pipe_);
  co_await closeHandle(pipe.get());
  BOOST_ASSERT(maybeError);
  throw std::move(maybeError.value());
}

void UnixStreamClient::ConnectAwaiter_::onConnect(uv_connect_t *req,
                                                  uv_status status) {
  auto *awaiter = getRequestDataOrNull<ConnectAwaiter_>(req);
  if (awaiter == nullptr) {
    delete req;
    return;
  }
  if (status == UV_ECANCELED) {
    return;
  }
  awaiter->status_ = status;
  if (awaiter->handle_) {
    Loop::enqueue(awaiter->handle_);
    awaiter->handle_ = nullptr;
  }
}

bool UnixStreamClient::ConnectAwaiter_::await_ready() { return false; }

bool UnixStreamClient::ConnectAwaiter_::await_suspend(
    std::coroutine_handle<> handle) {
  BOOST_ASSERT(handle_ == nullptr);
  handle_ = handle;
  setRequestData(request_.get(), this);
  request_->data = this;
  return true;
}

UnixStream UnixStreamClient::ConnectAwaiter_::await_resume() {
  resetRequestData(request_.get());
  if (status_ != 0) {
    throw UvcoException{status_, "UnixStreamClient failed to connect"};
  }
  handle_ = nullptr;
  return UnixStream{std::move(pipe_)};
}

} // namespace uvco
