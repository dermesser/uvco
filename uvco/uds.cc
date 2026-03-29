// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <boost/assert.hpp>
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
#include "uvco/util.h"

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

/// An awaiter class used to wait for a connection to be established.
///
/// Implementation note: almost the entire mechanics of connecting is
/// handled by the awaiter. The `connect()` method is just a thin wrapper
/// around the awaiter; the awaiter's methods also throw exceptions. This
/// is different than e.g. in the `TcpClient` class.
struct UnixStreamClient::ConnectAwaiter_ {
  ConnectAwaiter_(const ConnectAwaiter_ &) = delete;
  ConnectAwaiter_(ConnectAwaiter_ &&) = delete;
  ConnectAwaiter_ &operator=(const ConnectAwaiter_ &) = delete;
  ConnectAwaiter_ &operator=(ConnectAwaiter_ &&) = delete;
  explicit ConnectAwaiter_(const Loop &loop);
  ~ConnectAwaiter_();

  static void onConnect(uv_connect_t *req, uv_status status);

  [[nodiscard]] static bool await_ready();
  bool await_suspend(std::coroutine_handle<> handle);
  UnixStream await_resume();

  std::unique_ptr<uv_connect_t> req_;
  std::unique_ptr<uv_pipe_t> pipe_;
  std::coroutine_handle<> handle_;
  uv_status status_{};
};

UnixStreamClient::ConnectAwaiter_::ConnectAwaiter_(const Loop &loop)
    : req_{std::make_unique<uv_connect_t>()},
      pipe_{std::make_unique<uv_pipe_t>()} {
  uv_pipe_init(loop.uvloop(), pipe_.get(), 0);
  setRequestData(req_.get(), this);
}

UnixStreamClient::ConnectAwaiter_::~ConnectAwaiter_() {
  // request data is reset by await_resume(), so if it's non-null, the
  // connection attempt was cancelled and we need to clean up.
  if (pipe_ != nullptr) {
    closeHandle(pipe_.release());
  }
  // Request object is freed by onConnect
}

Promise<UnixStream> UnixStreamClient::connect(std::string_view path) {
  ConnectAwaiter_ awaiter{loop_};
  const OnExit onExit{[&awaiter, req = awaiter.req_.get()] {
    if (awaiter.handle_ != nullptr) {
      resetRequestData(req);
    }
  }};

#if UV_VERSION_MAJOR == 1 && UV_VERSION_MINOR >= 46
  const uv_status connectStatus =
      uv_pipe_connect2(awaiter.req_.release(), awaiter.pipe_.get(),
                       path.data(), path.size(), 0, ConnectAwaiter_::onConnect);
  if (connectStatus != 0) {
    closeHandle(awaiter.pipe_.release());
    throw UvcoException(connectStatus,
                        "UnixStreamClient::connect() failed immediately");
  }
#else
  uv_pipe_connect(connect.request_.release(), connect.pipe_.get(), path.data(),
                  ConnectAwaiter_::onConnect);
#endif

  std::optional<UvcoException> maybeError;
  // Special error handling mechanism to properly close the
  // open but not connected handle.
  try {
    co_return (co_await awaiter);
  } catch (const UvcoException &e) {
    maybeError = e;
  }
  closeHandle(awaiter.pipe_.release());
  BOOST_ASSERT(maybeError);
  throw std::move(maybeError.value());
}

void UnixStreamClient::ConnectAwaiter_::onConnect(uv_connect_t *req,
                                                  uv_status status) {
  const std::unique_ptr<uv_connect_t> reqPtr{req};
  auto *awaiter = getRequestDataOrNull<ConnectAwaiter_>(reqPtr.get());
  if (awaiter == nullptr) {
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
  return true;
}

UnixStream UnixStreamClient::ConnectAwaiter_::await_resume() {
  if (status_ != 0) {
    throw UvcoException{status_, "UnixStreamClient failed to connect"};
  }
  handle_ = nullptr;
  return UnixStream{std::move(pipe_)};
}

} // namespace uvco
