// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <boost/assert.hpp>
#include <fmt/core.h>
#include <uv.h>
#include <uv/unix.h>

#include "uvco/close.h"
#include "uvco/exception.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"
#include "uvco/stream.h"

#include <array>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace uvco {

StreamBase::~StreamBase() {
  // close() MUST be called and awaited before dtor.
  if (stream_) {
    fmt::print(stderr, "StreamBase::~StreamBase(): closing stream in dtor; "
                       "this will leak memory. "
                       "Please co_await stream.close() if possible.\n");
    // Asynchronously close handle. It's better to leak memory than file
    // descriptors.
    closeHandle(stream_.release());
  }
}

TtyStream TtyStream::tty(const Loop &loop, int fd) {
  auto tty = std::make_unique<uv_tty_t>();
  uv_status status = uv_tty_init(loop.uvloop(), tty.get(), fd, 0);
  if (status != 0) {
    throw UvcoException(status,
                        fmt::format("opening TTY with fd {} failed", fd));
  }
  return TtyStream{std::move(tty)};
}

Promise<std::optional<std::string>> StreamBase::read() {
  // This is a promise root function, i.e. origin of a promise.
  InStreamAwaiter_ awaiter{*this};
  std::optional<std::string> buf = co_await awaiter;
  co_return buf;
}

Promise<uv_status> StreamBase::write(std::string buf) {
  OutStreamAwaiter_ awaiter{*this, std::move(buf)};
  uv_status status = co_await awaiter;
  co_return status;
}

Promise<void> StreamBase::shutdown() {
  uv_shutdown_t shutdownReq;
  shutdownReq.handle = &stream();
  ShutdownAwaiter_ awaiter{shutdownReq};
  co_await awaiter;
  co_return;
}

Promise<void> StreamBase::close() {
  auto stream = std::move(stream_);
  co_await closeHandle(stream.get());
  if (reader_) {
    const auto reader = *reader_;
    reader_.reset();
    Loop::enqueue(reader);
  }
  if (writer_) {
    const auto writer = *writer_;
    writer_.reset();
    Loop::enqueue(writer);
  }
}

bool StreamBase::InStreamAwaiter_::await_ready() {
  uv_status state = uv_is_readable(&stream_.stream());
  if (state == 1) {
    // Read available data and return immediately.
    start_read();
    stop_read();
  }
  return slot_.has_value();
}

bool StreamBase::InStreamAwaiter_::await_suspend(
    std::coroutine_handle<> handle) {
  stream_.stream().data = this;
  handle_ = handle;
  stream_.reader_ = handle;
  start_read();
  return true;
}

std::optional<std::string> StreamBase::InStreamAwaiter_::await_resume() {
  if (!slot_ && !stream_.stream_) {
    return {};
  }
  BOOST_ASSERT(slot_);
  std::optional<std::string> result = std::move(*slot_);
  slot_.reset();
  stream_.reader_.reset();
  return result;
}

void StreamBase::InStreamAwaiter_::start_read() {
  uv_read_start(&stream_.stream(), allocator, onInStreamRead);
}

void StreamBase::InStreamAwaiter_::stop_read() {
  uv_read_stop(&stream_.stream());
}

void StreamBase::InStreamAwaiter_::onInStreamRead(uv_stream_t *stream,
                                                  ssize_t nread,
                                                  const uv_buf_t *buf) {
  auto *awaiter = (InStreamAwaiter_ *)stream->data;
  awaiter->stop_read();

  if (nread == UV_EOF) {
    awaiter->slot_ = std::optional<std::string>{};
  } else if (nread >= 0) {
    std::string line{buf->base, static_cast<size_t>(nread)};
    awaiter->slot_ = std::move(line);
  } else {
    // Some error; assume EOF.
    awaiter->slot_ = std::optional<std::string>{};
  }

  freeUvBuf(buf);
  if (awaiter->handle_) {
    auto handle = awaiter->handle_.value();
    awaiter->handle_.reset();
    Loop::enqueue(handle);
  }
}

StreamBase::OutStreamAwaiter_::OutStreamAwaiter_(StreamBase &stream,
                                                 std::string &&buffer)
    : stream_{stream}, buffer_{std::move(buffer)}, write_{} {}
std::array<uv_buf_t, 1> StreamBase::OutStreamAwaiter_::prepare_buffers() const {
  std::array<uv_buf_t, 1> bufs{};
  bufs[0].base = const_cast<char *>(buffer_.c_str());
  bufs[0].len = buffer_.size();
  return bufs;
}

bool StreamBase::OutStreamAwaiter_::await_ready() {
  // Attempt early write:
  auto bufs = prepare_buffers();
  uv_status result = uv_try_write(&stream_.stream(), bufs.data(), bufs.size());
  if (result > 0) {
    status_ = result;
  }
  return result > 0;
}

bool StreamBase::OutStreamAwaiter_::await_suspend(
    std::coroutine_handle<> handle) {
  write_.data = this;
  handle_ = handle;
  // For resumption during close.
  stream_.writer_ = handle;
  auto bufs = prepare_buffers();
  // TODO: move before suspension point.
  uv_write(&write_, &stream_.stream(), bufs.data(), bufs.size(),
           onOutStreamWrite);

  return true;
}

uv_status StreamBase::OutStreamAwaiter_::await_resume() {
  // resumed due to close.
  if (!stream_.stream_) {
    return UV_ECANCELED;
  }
  BOOST_ASSERT(status_);
  stream_.writer_.reset();
  return *status_;
}

void StreamBase::OutStreamAwaiter_::onOutStreamWrite(uv_write_t *write,
                                                     uv_status status) {
  auto *awaiter = (OutStreamAwaiter_ *)write->data;
  awaiter->status_ = status;
  BOOST_ASSERT(awaiter->handle_);
  auto handle = awaiter->handle_.value();
  awaiter->handle_.reset();
  Loop::enqueue(handle);
}

bool StreamBase::ShutdownAwaiter_::await_ready() { return false; }

bool StreamBase::ShutdownAwaiter_::await_suspend(
    std::coroutine_handle<> handle) {
  BOOST_ASSERT(!handle_);
  handle_ = handle;

  req_.data = this;
  uv_shutdown(&req_, req_.handle, StreamBase::ShutdownAwaiter_::onShutdown);
  return true;
}

void StreamBase::ShutdownAwaiter_::await_resume() {
  BOOST_ASSERT(status_);
  if (status_ && *status_ != 0) {
    throw UvcoException{*status_, "StreamBase::shutdown() encountered error"};
  }
}

void StreamBase::ShutdownAwaiter_::onShutdown(uv_shutdown_t *req,
                                              uv_status status) {
  auto *awaiter = (ShutdownAwaiter_ *)req->data;
  awaiter->status_ = status;
  BOOST_ASSERT(awaiter->handle_);
  Loop::enqueue(*awaiter->handle_);
}

} // namespace uvco
