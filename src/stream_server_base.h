// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include "exception.h"
#include "internal/internal_utils.h"
#include "loop/loop.h"
#include "promise/multipromise.h"
#include "promise/promise.h"

#include <coroutine>
#include <cstdio>
#include <fmt/core.h>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace uvco {

/// @addtogroup StreamServer
/// @{

/// Helper template class to statically dispatch to the different uv_xyz_init()
/// functions.
template <typename UvStreamType> struct UvStreamInitHelper {
  static void init(uv_loop_t * /*loop*/, UvStreamType * /*stream*/) {
    BOOST_ASSERT_MSG(false, "UvStreamInit not specialized for this type");
  }
};

/// Not for use in user code; base class for e.g. UnixServer and TcpServer.
///
/// Because accepting connections looks the same for Unix and TCP servers, the
/// behavior is defined here and shared by both. However, the implementation
/// must be generic over the stream type, so the actual stream type is a
/// template parameter.
template <typename UvStreamType, typename StreamType> class StreamServerBase {
public:
  StreamServerBase(const StreamServerBase &) = delete;
  StreamServerBase(StreamServerBase &&) = default;
  StreamServerBase &operator=(const StreamServerBase &) = delete;
  StreamServerBase &operator=(StreamServerBase &&) = default;
  ~StreamServerBase();

  /// Return client connections as clients connect.
  ///
  /// Raises exceptions if errors occur during accepting or listening.
  ///
  /// This generator may not be `co_await`ed on after having called `close()`.
  MultiPromise<StreamType> listen(int backlog = 128);

  /// Close server and stop accepting client connections; must be awaited.
  Promise<void> close();

protected:
  explicit StreamServerBase(std::unique_ptr<UvStreamType> socket)
      : socket_{std::move(socket)} {}
  std::unique_ptr<UvStreamType> socket_;

private:
  static void onNewConnection(uv_stream_t *stream, uv_status status);

  struct ConnectionAwaiter_ {
    explicit ConnectionAwaiter_(UvStreamType &socket) : socket_{socket} {
      accepted_.reserve(4);
    }
    [[nodiscard]] bool await_ready() const;
    bool await_suspend(std::coroutine_handle<> handle);
    // Returns true if one or more connections were accepted.
    // Returns false if the listener should stop.
    [[nodiscard]] bool await_resume() const { return !stopped_; }

    /// Stop a listener coroutine.
    void stop();

    UvStreamType &socket_;
    std::optional<std::coroutine_handle<>> handle_;

    // Set of accepted connections or errors.
    using Accepted = std::variant<uv_status, StreamType>;
    std::vector<Accepted> accepted_;

    bool stopped_ = false;
  };
};

template <typename UvStreamType, typename StreamType>
StreamServerBase<UvStreamType, StreamType>::~StreamServerBase() {
  if (socket_) {
    fmt::print(stderr, "StreamServerBase::~StreamServerBase(): closing server "
                       "in dtor; this will leak memory. "
                       "Please co_await server.close() if possible.\n");
    // Asynchronously close handle. It's better to leak memory than file
    // descriptors.
    closeHandle(socket_.release());
  }
}

template <typename UvStreamType, typename StreamType>
Promise<void> StreamServerBase<UvStreamType, StreamType>::close() {
  auto *awaiter = (ConnectionAwaiter_ *)socket_->data;
  // Resume listener coroutine and tell it to exit.
  // If awaiter == nullptr, one of two things is true:
  // 1. listener is currently not running
  // 2. listener has yielded and is suspended there: the listener generator will
  // be cancelled when its MultiPromise is dropped.
  if (awaiter != nullptr && awaiter->handle_) {
    awaiter->stop();
  }
  co_await closeHandle(socket_.get());
  socket_.reset();
}

template <typename UvStreamType, typename StreamType>
MultiPromise<StreamType>
StreamServerBase<UvStreamType, StreamType>::listen(int backlog) {
  BOOST_ASSERT(socket_);
  ConnectionAwaiter_ awaiter{*socket_};
  socket_->data = &awaiter;

  const uv_status listenStatus =
      uv_listen((uv_stream_t *)socket_.get(), backlog,
                StreamServerBase<UvStreamType, StreamType>::onNewConnection);
  if (listenStatus != 0) {
    socket_->data = nullptr;
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
        socket_->data = nullptr;
        throw UvcoException{status,
                            "UnixStreamServer failed to accept a connection!"};
      } else {
        // Awkward handlign: if the returned MultiPromise is dropped, we will
        // never return from co_yield. However, dropping may happen after
        // calling `close()`, so we cannot rely on socket_ still existing.
        //
        // `close()` also relies on whether `socket_->data` is `nullptr` or not
        // to decide if the socket has been closed already.
        socket_->data = nullptr;
        co_yield std::move(std::get<1>(streamSlot));
        socket_->data = &awaiter;
      }
    }
    awaiter.accepted_.clear();
  }
  socket_->data = nullptr;
}

template <typename UvStreamType, typename StreamType>
void StreamServerBase<UvStreamType, StreamType>::ConnectionAwaiter_::stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  if (handle_) {
    // Synchronous resume to ensure that the listener is stopped by the time
    // the function returns.
    const auto handle = *handle_;
    handle_.reset();
    handle.resume();
  }
}

template <typename UvStreamType, typename StreamType>
bool StreamServerBase<UvStreamType, StreamType>::ConnectionAwaiter_::
    await_suspend(std::coroutine_handle<> handle) {
  BOOST_ASSERT(!handle_);
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
  auto *connectionAwaiter = (ConnectionAwaiter_ *)server->data;
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

  if (connectionAwaiter->handle_) {
    Loop::enqueue(*connectionAwaiter->handle_);
    connectionAwaiter->handle_.reset();
  }
}

/// @}

} // namespace uvco
