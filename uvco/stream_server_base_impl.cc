// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

/// @file stream_server_base_impl.cc
///
/// Definition and instantiations of the StreamServerBase<> class template.
/// Because only two instantiations are needed, the template is defined in a
/// separate file to reduce compilation time.

#include <boost/assert.hpp>
#include <fmt/core.h>
#include <uv.h>

#include "uvco/close.h"
#include "uvco/exception.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/loop/loop.h"
#include "uvco/name_resolution.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"
#include "uvco/stream_server_base.h"
#include "uvco/tcp_stream.h"
#include "uvco/uds_stream.h"

#include <coroutine>
#include <memory>
#include <utility>

namespace uvco {

template <typename UvStreamType, typename StreamType>
struct StreamServerBase<UvStreamType, StreamType>::ConnectionAwaiter_ {
  explicit ConnectionAwaiter_(UvStreamType &socket) : socket_{socket} {
    accepted_.reserve(4);
    setData(&socket_, this);
  }
  ~ConnectionAwaiter_() { resetData(&socket_); }

  [[nodiscard]] bool await_ready() const;
  bool await_suspend(std::coroutine_handle<> handle);
  // Returns true if one or more connections were accepted.
  // Returns false if the listener should stop.
  [[nodiscard]] bool await_resume() const { return !stopped_; }

  /// Stop a listener coroutine.
  void stop();

  UvStreamType &socket_;
  std::coroutine_handle<> handle_;

  // Set of accepted connections or errors.
  using Accepted = std::variant<uv_status, StreamType>;
  std::vector<Accepted> accepted_;

  bool stopped_ = false;
};

template <typename UvStreamType, typename StreamType>
void StreamServerBase<UvStreamType, StreamType>::ConnectionAwaiter_::stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  if (handle_ != nullptr) {
    // Synchronous resume to ensure that the listener is stopped by the time
    // the function returns.
    const std::coroutine_handle<> handle = handle_;
    handle_ = nullptr;
    handle.resume();
  }
}

template <typename UvStreamType, typename StreamType>
bool StreamServerBase<UvStreamType, StreamType>::ConnectionAwaiter_::
    await_suspend(std::coroutine_handle<> handle) {
  BOOST_ASSERT(handle_ == nullptr);
  handle_ = handle;
  return true;
}

template <typename UvStreamType, typename StreamType>
bool StreamServerBase<UvStreamType,
                      StreamType>::ConnectionAwaiter_::await_ready() const {
  return !accepted_.empty();
}

template <typename UvStreamType, typename StreamType>
void StreamServerBase<UvStreamType, StreamType>::onNewConnection(
    uv_stream_t *stream, uv_status status) {
  const auto *server = (UvStreamType *)stream;
  auto *connectionAwaiter = getData<ConnectionAwaiter_>(server);
  uv_loop_t *const loop = connectionAwaiter->socket_.loop;

  if (status == 0) {
    auto clientStream = std::make_unique<UvStreamType>();
    UvStreamInitHelper<UvStreamType>::init(loop, clientStream.get());
    const uv_status acceptStatus =
        uv_accept((uv_stream_t *)server, (uv_stream_t *)clientStream.get());
    if (acceptStatus == 0) {
      connectionAwaiter->accepted_.emplace_back(
          StreamType{std::move(clientStream)});
    } else {
      connectionAwaiter->accepted_.emplace_back(acceptStatus);
    }
  } else {
    connectionAwaiter->accepted_.emplace_back(status);
  }

  if (connectionAwaiter->handle_ != nullptr) {
    Loop::enqueue(connectionAwaiter->handle_);
    connectionAwaiter->handle_ = nullptr;
  }
}

template <typename UvStreamType, typename StreamType>
StreamServerBase<UvStreamType, StreamType>::~StreamServerBase() {
  // Quasi-sync method.
  close();
}

template <typename UvStreamType, typename StreamType>
void StreamServerBase<UvStreamType, StreamType>::close() {
  if (socket_ != nullptr && !dataIsNull(socket_.get())) {
    auto *awaiter = getData<ConnectionAwaiter_>(socket_.get());
    // Resume listener coroutine and tell it to exit.
    // If awaiter == nullptr, one of two things is true:
    // 1. listener is currently not running
    // 2. listener has yielded and is suspended there: the listener generator
    // will be cancelled when its MultiPromise is dropped.
    awaiter->stop();
  }
  if (socket_ != nullptr && !isClosed(socket_.get())) {
    // closeHandle takes care of freeing the memory if its own promise is
    // dropped.
    closeHandle(socket_.release());
  }
}

template <typename UvStreamType, typename StreamType>
MultiPromise<StreamType>
StreamServerBase<UvStreamType, StreamType>::listen(int backlog) {
  BOOST_ASSERT(socket_);
  ConnectionAwaiter_ awaiter{*socket_};

  const uv_status listenStatus =
      uv_listen((uv_stream_t *)socket_.get(), backlog,
                StreamServerBase<UvStreamType, StreamType>::onNewConnection);
  if (listenStatus != 0) {
    throw UvcoException{listenStatus,
                        "StreamServerBase::listen(): failed to listen"};
  }

  while (true) {
    const bool acceptOk = co_await awaiter;
    if (!acceptOk) {
      // At this point, do not touch socket_->data anymore!
      // This is the result of ConnectionAwaiter_::stop(), and
      // data points to a CloseAwaiter_ object.
      break;
    }

    for (auto it = awaiter.accepted_.begin(); it != awaiter.accepted_.end();
         it++) {
      auto &streamSlot = *it;

      if (streamSlot.index() == 0) {
        const uv_status status = std::get<0>(streamSlot);
        BOOST_ASSERT(status != 0);
        // When the error is handled, the user code can again call listen()
        // and will process the remaining connections. Therefore, first remove
        // the already processed connections.
        awaiter.accepted_.erase(awaiter.accepted_.begin(), it);
        throw UvcoException{status,
                            "UnixStreamServer failed to accept a connection!"};
      } else {
        // Awkward handlign: if the returned MultiPromise is dropped, we will
        // never return from co_yield. However, dropping may happen after
        // calling `close()`, so we cannot rely on socket_ still existing.
        //
        // `close()` also relies on whether `socket_->data` is `nullptr` or not
        // to decide if the socket has been closed already.
        co_yield std::move(std::get<1>(streamSlot));
      }
    }
    awaiter.accepted_.clear();
  }
}

template <typename UvStreamType, typename StreamType>
AddressHandle StreamServerBase<UvStreamType, StreamType>::getSockname() const {
  if constexpr (std::is_same_v<UvStreamType, uv_tcp_t>) {
    struct sockaddr_storage addr{};
    int addrLen = sizeof(addr);
    const uv_status status =
        uv_tcp_getsockname(socket_.get(), (struct sockaddr *)&addr, &addrLen);
    if (status != 0) {
      throw UvcoException{status, "StreamServerBase::getSockname() failed"};
    }
    return AddressHandle{(struct sockaddr *)&addr};
  } else if constexpr (std::is_same_v<UvStreamType, uv_pipe_t>) {
    throw UvcoException{
        UV_ENOSYS,
        "StreamServerBase::getSockname() not implemented for uv_pipe_t"};
  } else {
    static_assert(false);
  }
}

// Pre-instantiate templates for the known users of this class.
// Saves compilation time.

template <> struct UvStreamInitHelper<uv_tcp_t> {
  static void init(uv_loop_t *loop, uv_tcp_t *handle) {
    uv_tcp_init(loop, handle);
  }
};

template <> struct UvStreamInitHelper<uv_pipe_t> {
  static void init(uv_loop_t *loop, uv_pipe_t *handle) {
    uv_pipe_init(loop, handle, 0);
  }
};

template class StreamServerBase<uv_tcp_t, TcpStream>;
template class StreamServerBase<uv_pipe_t, UnixStream>;

} // namespace uvco
