// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <boost/assert.hpp>
#include <uv.h>

#include "close.h"
#include "stream.h"

namespace uvco {

StreamBase::~StreamBase() {
  // close() MUST be called and awaited before dtor.
  BOOST_ASSERT(!stream_);
}

TtyStream TtyStream::tty(uv_loop_t *loop, int fd) {
  auto *tty = new uv_tty_t{};
  uv_status status = uv_tty_init(loop, tty, fd, 0);
  if (status != 0)
    throw UvcoException(
        fmt::format("opening TTY failed: {}", uv_err_name(status)));
  auto *stream = (uv_stream_t *)tty;
  return TtyStream(stream);
}

Promise<std::optional<std::string>> StreamBase::read() {
  // This is a promise root function, i.e. origin of a promise.
  InStreamAwaiter_ awaiter{*this};
  std::optional<std::string> buf = co_await awaiter;
  co_return buf;
}

Promise<StreamBase::uv_status> StreamBase::write(std::string buf) {
  OutStreamAwaiter_ awaiter{*this, std::move(buf)};
  uv_status status = co_await awaiter;
  co_return status;
}

Promise<void> StreamBase::close() {
  co_await closeHandle(&stream());
  stream_.reset();
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
  start_read();
  return true;
}

std::optional<std::string> StreamBase::InStreamAwaiter_::await_resume() {
  BOOST_ASSERT(slot_);
  std::optional<std::string> result = std::move(*slot_);
  slot_.reset();
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

  if (nread >= 0) {
    std::string line{buf->base, static_cast<size_t>(nread)};
    awaiter->slot_ = std::move(line);
  } else {
    // Some error; assume EOF.
    awaiter->slot_ = std::optional<std::string>{};
  }

  freeUvBuf(buf);
  if (awaiter->handle_) {
    awaiter->handle_->resume();
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
  auto bufs = prepare_buffers();
  // TODO: move before suspension point.
  uv_write(&write_, &stream_.stream(), bufs.data(), bufs.size(),
           onOutStreamWrite);

  return true;
}

StreamBase::uv_status StreamBase::OutStreamAwaiter_::await_resume() {
  BOOST_ASSERT(status_);
  return *status_;
}

void StreamBase::OutStreamAwaiter_::onOutStreamWrite(uv_write_t *write,
                                                     uv_status status) {
  auto *state = (OutStreamAwaiter_ *)write->data;
  state->status_ = status;
  BOOST_ASSERT(state->handle_);
  state->handle_->resume();
}

} // namespace uvco
