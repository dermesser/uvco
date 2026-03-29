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
#include "uvco/util.h"

#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace uvco {

struct StreamBase::ShutdownAwaiter_ {
  ShutdownAwaiter_() = default;
  static void onShutdown(uv_shutdown_t *req, uv_status status);

  bool await_ready();
  bool await_suspend(std::coroutine_handle<> handle);
  void await_resume();

  std::coroutine_handle<> handle_;
  std::optional<uv_status> status_;
};

struct StreamBase::InStreamAwaiter_ {
  explicit InStreamAwaiter_(StreamBase &stream, std::span<char> buffer)
      : stream_{stream}, buffer_{buffer} {}
  ~InStreamAwaiter_();

  bool await_ready();
  bool await_suspend(std::coroutine_handle<> handle);
  size_t await_resume();

  void start_read();
  void stop_read();

  static void allocate(uv_handle_t *handle, size_t suggested_size,
                       uv_buf_t *buf);
  static void onInStreamRead(uv_stream_t *stream, ssize_t nread,
                             const uv_buf_t *buf);

  StreamBase &stream_;
  std::span<char> buffer_;
  std::optional<ssize_t> status_;
  std::coroutine_handle<> handle_;
};

struct StreamBase::OutStreamAwaiter_ {
  OutStreamAwaiter_(StreamBase &stream, std::string buffer);
  ~OutStreamAwaiter_();

  bool await_ready();
  bool await_suspend(std::coroutine_handle<> handle);
  uv_status await_resume();

  static void onOutStreamWrite(uv_write_t *write, uv_status status);

  // A libuv write object with associated buffer, which may outlive
  // the this awaiter instance.
  struct UvWriteWithBuffer {
    uv_write_t write;
    std::string buffer;
  };

  std::coroutine_handle<> handle_;
  std::optional<uv_status> status_;

  // State necessary for both immediate and delayed writing.
  std::span<const char> buffer_;
  // Heap-allocated write request is not as efficient, but required to make this
  // cancellation-safe.
  std::unique_ptr<UvWriteWithBuffer> write_;
  StreamBase &stream_;
};

StreamBase::~StreamBase() { close(); }

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
  co_return (co_await awaiter);
}

Promise<void> StreamBase::write(std::string buf) {
  OutStreamAwaiter_ awaiter{*this, std::move(buf)};
  std::array<uv_buf_t, 1> bufs{};
  bufs[0] = uv_buf_init(const_cast<char *>(awaiter.write_->buffer.data()),
                        awaiter.write_->buffer.size());

  setData(awaiter.write_.get(), &awaiter);
  const OnExit _onExit{[write = awaiter.write_.get(), &awaiter] {
    // handle_ is reset by onOutStreamWrite callback. If it's non-null, it's
    // because write() was cancelled.
    if (awaiter.handle_) {
      resetRequestData(write);
    }
  }};

  const uv_status status =
      uv_write((uv_write_t *)(awaiter.write_.release()), &stream(), bufs.data(),
               bufs.size(), OutStreamAwaiter_::onOutStreamWrite);
  if (status < 0) {
    throw UvcoException{status,
                        "StreamBase::write() encountered error in uv_write"};
  }
  const uv_status completionStatus = co_await awaiter;
  // may be UV_ECANCELED if the stream has been dropped.
  if (completionStatus < 0) {
    throw UvcoException{
        status, "StreamBase::write() encountered error while awaiting write"};
  }
  co_return;
}

Promise<void> StreamBase::shutdown() {
  auto shutdownReq = std::make_unique<uv_shutdown_t>();
  ShutdownAwaiter_ awaiter;
  const OnExit _onExit{[req = shutdownReq.get(), &awaiter] {
    if (awaiter.handle_) {
      resetRequestData(req);
    }
  }};

  setRequestData(shutdownReq.get(), &awaiter);
  uv_shutdown(shutdownReq.release(), &stream(),
              StreamBase::ShutdownAwaiter_::onShutdown);
  co_await awaiter;
  co_return;
}

void StreamBase::close() {
  if (stream_ != nullptr) {
    closeHandle(stream_.release());
  }
  if (reader_ != nullptr) {
    std::coroutine_handle<> reader = reader_;
    reader_ = nullptr;
    reader.resume();
  }
  if (writer_ != nullptr) {
    std::coroutine_handle<> writer = writer_;
    writer_ = nullptr;
    writer.resume();
  }
}

// Especially important for cancelled reads.
StreamBase::InStreamAwaiter_::~InStreamAwaiter_() {
  if (stream_.underlying() != nullptr &&
      1 == uv_is_active((const uv_handle_t *)stream_.underlying())) {
    stop_read();
  }
  // A cancelled read is signalled to the callback by a nullptr.
  if (stream_.stream_ != nullptr) {
    resetData(&stream_.stream());
  }
  stream_.reader_ = nullptr;
}

bool StreamBase::InStreamAwaiter_::await_ready() {
  const int state = uv_is_readable(&stream_.stream());
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
  BOOST_ASSERT(dataIsNull(&stream_.stream()));
  setData(&stream_.stream(), this);
  handle_ = handle;
  stream_.reader_ = handle;
  start_read();
  return true;
}

size_t StreamBase::InStreamAwaiter_::await_resume() {
  if (!status_ && !stream_.stream_) {
    return 0;
  }
  BOOST_ASSERT(status_);
  stream_.reader_ = nullptr;
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
  const InStreamAwaiter_ *awaiter = getData<InStreamAwaiter_>(handle);
  BOOST_ASSERT(awaiter != nullptr);
  *buf = uv_buf_init(const_cast<char *>(awaiter->buffer_.data()),
                     awaiter->buffer_.size());
}

void StreamBase::InStreamAwaiter_::start_read() {
  const uv_status result =
      uv_read_start(&stream_.stream(), StreamBase::InStreamAwaiter_::allocate,
                    StreamBase::InStreamAwaiter_::onInStreamRead);
  if (result != 0) {
    throw UvcoException{
        result, "StreamBase::read() encountered error in uv_read_start()"};
  }
}

void StreamBase::InStreamAwaiter_::stop_read() {
  // stream_.stream_ may be null if the stream has been closed in the meantime.
  if (stream_.stream_) {
    uv_read_stop(&stream_.stream());
  }
}

// buf is not used, because it is an alias to awaiter->buffer_.
void StreamBase::InStreamAwaiter_::onInStreamRead(uv_stream_t *stream,
                                                  ssize_t nread,
                                                  const uv_buf_t * /*buf*/) {
  auto *awaiter = getData<InStreamAwaiter_>(stream);
  BOOST_ASSERT(awaiter != nullptr);
  awaiter->stop_read();
  awaiter->status_ = nread;

  if (awaiter->handle_) {
    Loop::enqueue(awaiter->handle_);
    awaiter->handle_ = nullptr;
  }
  setData(stream, (void *)nullptr);
}

StreamBase::OutStreamAwaiter_::OutStreamAwaiter_(StreamBase &stream,
                                                 std::string buffer)
    : buffer_{buffer}, write_{std::make_unique<UvWriteWithBuffer>()},
      stream_{stream} {
  write_->buffer = std::move(buffer);
}

StreamBase::OutStreamAwaiter_::~OutStreamAwaiter_() {
  stream_.writer_ = nullptr;
}

bool StreamBase::OutStreamAwaiter_::await_ready() {
  // When at this point, we must suspend in any case.
  return false;
}

bool StreamBase::OutStreamAwaiter_::await_suspend(
    std::coroutine_handle<> handle) {
  handle_ = handle;
  // For resumption during close.
  stream_.writer_ = handle;

  return true;
}

uv_status StreamBase::OutStreamAwaiter_::await_resume() {
  // resumed due to close.
  if (!stream_.stream_) {
    return UV_ECANCELED;
  }
  BOOST_ASSERT(status_);
  stream_.writer_ = nullptr;
  return *status_;
}

void StreamBase::OutStreamAwaiter_::onOutStreamWrite(uv_write_t *write,
                                                     uv_status status) {
  const std::unique_ptr<UvWriteWithBuffer> req{(UvWriteWithBuffer *)write};
  auto *awaiter = getRequestDataOrNull<OutStreamAwaiter_>(req.get());
  if (awaiter == nullptr) {
    return;
  }
  BOOST_ASSERT(awaiter != nullptr);
  awaiter->status_ = status;
  BOOST_ASSERT(awaiter->handle_);
  Loop::enqueue(awaiter->handle_);
  awaiter->handle_ = nullptr;
  setData(write, (void *)nullptr);
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
  if (status_ && status_.value() != 0) {
    throw UvcoException{*status_, "StreamBase::shutdown() encountered error"};
  }
}

void StreamBase::ShutdownAwaiter_::onShutdown(uv_shutdown_t *req,
                                              uv_status status) {
  const std::unique_ptr<uv_shutdown_t> uniqueReq{req};
  auto *awaiter = getRequestDataOrNull<ShutdownAwaiter_>(uniqueReq.get());
  if (awaiter == nullptr) {
    // shutdown coroutine was ancelled.
    return;
  }
  awaiter->status_ = status;
  if (awaiter->handle_) {
    Loop::enqueue(awaiter->handle_);
    awaiter->handle_ = nullptr;
  }
}

[[nodiscard]] uv_os_fd_t StreamBase::fd() const {
  uv_os_fd_t fd{};
  const uv_status status = uv_fileno((uv_handle_t *)stream_.get(), &fd);
  if (status != 0) {
    throw UvcoException(status, "StreamBase::fd(): uv_fileno() failed: " +
                                    std::string{uv_strerror(status)});
  }
  return fd;
}

} // namespace uvco
