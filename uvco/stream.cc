// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <boost/assert.hpp>
#include <fmt/core.h>
#include <span>
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
#include <string_view>
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

Promise<std::optional<std::string>> StreamBase::read(size_t maxSize) {
  // This is a promise root function, i.e. origin of a promise.
  std::string buf(maxSize, '\0');
  InStreamAwaiter_ awaiter{*this, buf};
  const size_t nRead = co_await awaiter;
  if (nRead == 0) {
    // EOF.
    co_return std::nullopt;
  }
  buf.resize(nRead);
  co_return buf;
}

Promise<size_t> StreamBase::read(std::span<char> buffer) {
  InStreamAwaiter_ awaiter{*this, buffer};
  size_t n = co_await awaiter;
  co_return n;
}

Promise<size_t> StreamBase::write(std::string buf) {
  OutStreamAwaiter_ awaiter{*this, std::move(buf)};
  uv_status status = co_await awaiter;
  if (status < 0) {
    throw UvcoException{status, "StreamBase::write() encountered error"};
  }
  co_return static_cast<size_t>(status);
}

Promise<void> StreamBase::shutdown() {
  uv_shutdown_t shutdownReq;
  ShutdownAwaiter_ awaiter;

  shutdownReq.data = &awaiter;
  uv_shutdown(&shutdownReq, &stream(),
              StreamBase::ShutdownAwaiter_::onShutdown);
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
    // If data is available, the callback onInStreamRead will be called
    // immediately. In that case we don't have to wait.
    start_read();
    stop_read();
  }
  return status_.has_value();
}

bool StreamBase::InStreamAwaiter_::await_suspend(
    std::coroutine_handle<> handle) {
  BOOST_ASSERT(uv_handle_get_data((uv_handle_t *)&stream_.stream()) == nullptr);
  uv_handle_set_data((uv_handle_t *)&stream_.stream(), this);
  handle_ = handle;
  stream_.reader_ = handle;
  start_read();
  return true;
}

size_t StreamBase::InStreamAwaiter_::await_resume() {
  if (!status_ && !stream_.stream_) {
    return {};
  }
  BOOST_ASSERT(status_);
  stream_.reader_.reset();
  if (status_ && *status_ == UV_EOF) {
    return 0;
  }
  if (status_ && *status_ < 0) {
    throw UvcoException{static_cast<uv_status>(*status_),
                        "StreamBase::read() encountered error"};
  }
  BOOST_ASSERT(status_.value() >= 0);
  return static_cast<size_t>(status_.value());
}

// Provides the InStreamAwaiter_'s span buffer to libuv.
void StreamBase::InStreamAwaiter_::allocate(uv_handle_t *handle,
                                            size_t /*suggested_size*/,
                                            uv_buf_t *buf) {
  const InStreamAwaiter_ *awaiter = (InStreamAwaiter_ *)handle->data;
  BOOST_ASSERT(awaiter != nullptr);
  buf->base = awaiter->buffer_.data();
  buf->len = awaiter->buffer_.size();
}

void StreamBase::InStreamAwaiter_::start_read() {
  uv_read_start(&stream_.stream(), StreamBase::InStreamAwaiter_::allocate,
                StreamBase::InStreamAwaiter_::onInStreamRead);
}

void StreamBase::InStreamAwaiter_::stop_read() {
  uv_read_stop(&stream_.stream());
}

// buf is not used, because it is an alias to awaiter->buffer_.
void StreamBase::InStreamAwaiter_::onInStreamRead(uv_stream_t *stream,
                                                  ssize_t nread,
                                                  const uv_buf_t * /*buf*/) {
  auto *awaiter = (InStreamAwaiter_ *)stream->data;
  BOOST_ASSERT(awaiter != nullptr);
  awaiter->stop_read();
  awaiter->status_ = nread;

  if (awaiter->handle_) {
    auto handle = awaiter->handle_.value();
    awaiter->handle_.reset();
    Loop::enqueue(handle);
  }
  stream->data = nullptr;
}

StreamBase::OutStreamAwaiter_::OutStreamAwaiter_(StreamBase &stream,
                                                 std::string_view buffer)
    : buffer_{buffer}, write_{}, stream_{stream} {}

std::array<uv_buf_t, 1> StreamBase::OutStreamAwaiter_::prepare_buffers() const {
  std::array<uv_buf_t, 1> bufs{};
  bufs[0].base = const_cast<char *>(buffer_.data());
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
  BOOST_ASSERT(write_.data == nullptr);
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
  BOOST_ASSERT(awaiter != nullptr);
  awaiter->status_ = status;
  BOOST_ASSERT(awaiter->handle_);
  auto handle = awaiter->handle_.value();
  awaiter->handle_.reset();
  Loop::enqueue(handle);
  write->data = nullptr;
}

bool StreamBase::ShutdownAwaiter_::await_ready() { return false; }

bool StreamBase::ShutdownAwaiter_::await_suspend(
    std::coroutine_handle<> handle) {
  BOOST_ASSERT(!handle_);
  handle_ = handle;
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
  if (awaiter->handle_) {
    auto handle = awaiter->handle_.value();
    awaiter->handle_.reset();
    Loop::enqueue(handle);
  }
}

} // namespace uvco
