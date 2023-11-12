#include "stream.h"

#include "close.h"

namespace uvco {

Stream::~Stream() {
  // close() MUST be called and awaited before dtor.
  assert(!stream_);
}

Stream Stream::tty(uv_loop_t *loop, int fd) {
  auto *tty = new uv_tty_t{};
  int status = uv_tty_init(loop, tty, fd, 0);
  if (status != 0)
    throw UvcoException(
        fmt::format("opening TTY failed: {}", uv_err_name(status)));
  auto *stream = (uv_stream_t *)tty;
  return Stream(stream);
}

Promise<std::optional<std::string>> Stream::read() {
  // This is a promise root function, i.e. origin of a promise.
  InStreamAwaiter_ awaiter{*this};
  std::optional<std::string> buf = co_await awaiter;
  co_return buf;
}

Promise<Stream::uv_status> Stream::write(std::string buf) {
  OutStreamAwaiter_ awaiter{*this, std::move(buf)};
  uv_status status = co_await awaiter;
  co_return status;
}

Promise<void> Stream::close(void (*uv_close_impl)(uv_handle_t *, uv_close_cb)) {
  // TODO: schedule closing operation on event loop?
  CloseAwaiter awaiter{};

  stream_->data = &awaiter;
  uv_close_impl((uv_handle_t *)stream_.get(), onCloseCallback);
  co_await awaiter;
  stream_.reset();
}

bool Stream::InStreamAwaiter_::await_ready() {
  int state = uv_is_readable(stream_.stream_.get());
  if (state == 1) {
    // Read available data and return immediately.
    start_read();
    stop_read();
  }
  return slot_.has_value();
}

bool Stream::InStreamAwaiter_::await_suspend(std::coroutine_handle<> handle) {
  stream_.stream_->data = this;
  handle_ = handle;
  start_read();
  return true;
}

std::optional<std::string> Stream::InStreamAwaiter_::await_resume() {
  assert(slot_);
  return std::move(*slot_);
}

void Stream::InStreamAwaiter_::start_read() {
  uv_read_start(stream_.stream_.get(), allocator, onInStreamRead);
}

void Stream::InStreamAwaiter_::stop_read() {
  uv_read_stop(stream_.stream_.get());
}

void Stream::InStreamAwaiter_::onInStreamRead(uv_stream_t *stream,
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

  if (awaiter->handle_) {
    awaiter->handle_->resume();
  }

  freeUvBuf(buf);
}

Stream::OutStreamAwaiter_::OutStreamAwaiter_(Stream &stream,
                                             std::string &&buffer)
    : stream_{stream}, buffer_{std::move(buffer)}, write_{} {}
std::array<uv_buf_t, 1> Stream::OutStreamAwaiter_::prepare_buffers() const {
  std::array<uv_buf_t, 1> bufs{};
  bufs[0].base = const_cast<char *>(buffer_.c_str());
  bufs[0].len = buffer_.size();
  return bufs;
}

bool Stream::OutStreamAwaiter_::await_ready() {
  // Attempt early write:
  auto bufs = prepare_buffers();
  int result = uv_try_write(stream_.stream_.get(), bufs.data(), bufs.size());
  if (result > 0)
    status_ = result;
  return result > 0;
}

bool Stream::OutStreamAwaiter_::await_suspend(std::coroutine_handle<> handle) {
  write_.data = this;
  handle_ = handle;
  auto bufs = prepare_buffers();
  // TODO: move before suspension point.
  uv_write(&write_, stream_.stream_.get(), bufs.data(), bufs.size(),
           onOutStreamWrite);

  return true;
}

Stream::uv_status Stream::OutStreamAwaiter_::await_resume() {
  assert(status_);
  return *status_;
}

void Stream::OutStreamAwaiter_::onOutStreamWrite(uv_write_t *write,
                                                 int status) {
  auto *state = (OutStreamAwaiter_ *)write->data;
  state->status_ = status;
  assert(state->handle_);
  state->handle_->resume();
}

} // namespace uvco
