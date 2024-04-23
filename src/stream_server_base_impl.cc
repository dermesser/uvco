// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

/// @file stream_server_base_impl.cc
///
/// Definition and instantiations of the StreamServerBase<> class template.
/// Because only two instantiations are needed, the template is defined in a
/// separate file to reduce compilation time.

#include <boost/assert.hpp>
#include <fmt/core.h>
#include <uv.h>

#include "close.h"
#include "exception.h"
#include "internal/internal_utils.h"
#include "loop/loop.h"
#include "promise/multipromise.h"
#include "promise/promise.h"
#include "stream_server_base.h"
#include "tcp_stream.h"
#include "uds_stream.h"

#include <coroutine>
#include <cstdio>
#include <memory>
#include <utility>

namespace uvco {

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
