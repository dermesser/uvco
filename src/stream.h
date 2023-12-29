// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include <boost/assert.hpp>

#include "internal/internal_utils.h"
#include "promise/promise.h"

#include <array>
#include <coroutine>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace uvco {

/// @addtogroup Stream
/// @{

/// A plain stream, permitting reading, writing, and closing.
class StreamBase {
public:
  /// Takes ownership of stream.
  explicit StreamBase(uv_stream_t *stream) : stream_{stream} {}
  StreamBase(const StreamBase &) = delete;
  StreamBase(StreamBase &&) = default;
  StreamBase &operator=(const StreamBase &) = delete;
  StreamBase &operator=(StreamBase &&) = default;
  virtual ~StreamBase();

  /// Read available data (up to 4 kB) from stream. Returns an empty optional on
  /// EOF or closed handle (`close()`).
  [[nodiscard]] Promise<std::optional<std::string>> read();

  /// Write a buffer to the stream. A copy of `buf` is taken because it is
  /// undetermined when the actual write will occur. Await the result if the
  /// status is important; the write will be executed even without awaiting (as
  /// long as the process keeps running).
  [[nodiscard]] Promise<uv_status> write(std::string buf);

  /// The result of `close()` *must be `co_await`ed*; otherwise memory may be
  /// leaked. (this is not an issue just before termination of a process)
  ///
  /// Informs pending readers and writers of the close and causes them to return
  /// an empty optional.
  [[nodiscard]] Promise<void> close();

  /// Return the underlying UV stream object.
  [[nodiscard]] const uv_stream_t *underlying() const { return stream_.get(); }

protected:
  uv_stream_t &stream() {
    BOOST_ASSERT(stream_);
    return *stream_;
  }
  void destroyStream() {
    BOOST_ASSERT(stream_);
    stream_.reset();
  }

private:
  std::unique_ptr<uv_stream_t, UvHandleDeleter> stream_;
  std::optional<std::coroutine_handle<>> reader_;
  std::optional<std::coroutine_handle<>> writer_;

  struct InStreamAwaiter_ {
    explicit InStreamAwaiter_(StreamBase &stream) : stream_{stream} {}

    bool await_ready();
    bool await_suspend(std::coroutine_handle<> handle);
    std::optional<std::string> await_resume();

    void start_read();
    void stop_read();

    static void onInStreamRead(uv_stream_t *stream, ssize_t nread,
                               const uv_buf_t *buf);

    StreamBase &stream_;
    std::optional<std::optional<std::string>> slot_;
    std::optional<std::coroutine_handle<>> handle_;
  };

  struct OutStreamAwaiter_ {
    OutStreamAwaiter_(StreamBase &stream, std::string &&buffer);

    std::array<uv_buf_t, 1> prepare_buffers() const;

    bool await_ready();

    bool await_suspend(std::coroutine_handle<> handle);

    uv_status await_resume();

    static void onOutStreamWrite(uv_write_t *write, uv_status status);

    StreamBase &stream_;
    std::optional<std::coroutine_handle<>> handle_;

    std::string buffer_;
    uv_write_t write_{};
    std::optional<uv_status> status_;
  };
};

/// A stream referring to stdin/stdout/stderr. Should be created using one of
/// the static member functions, each of which creates a TTY stream referring to
/// the respective standard stream.
///
/// Note: these must be TTYs; input/output redirection may cause failures in
/// libuv. Normal files are not yet implemented.
class TtyStream : public StreamBase {
public:
  // Takes ownership of stream.
  TtyStream(TtyStream &&other) = default;
  TtyStream(const TtyStream &other) = delete;
  TtyStream &operator=(TtyStream &&) = default;
  TtyStream &operator=(const TtyStream &) = delete;
  ~TtyStream() override = default;

  static TtyStream tty(uv_loop_t *loop, int fd);
  static TtyStream stdin(uv_loop_t *loop) { return tty(loop, 0); }
  static TtyStream stdout(uv_loop_t *loop) { return tty(loop, 1); }
  static TtyStream stderr(uv_loop_t *loop) { return tty(loop, 2); }

private:
  explicit TtyStream(uv_stream_t *stream) : StreamBase{stream} {}
};

/// @}

} // namespace uvco
