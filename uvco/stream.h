// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <boost/assert.hpp>
#include <fmt/core.h>
#include <span>
#include <string_view>
#include <uv.h>
#include <uv/unix.h>

#include "uvco/close.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"

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
  template <typename Stream>
  explicit StreamBase(std::unique_ptr<Stream> stream)
      : stream_{(uv_stream_t *)stream.release()} {}
  StreamBase(const StreamBase &) = delete;
  StreamBase(StreamBase &&) = default;
  StreamBase &operator=(const StreamBase &) = delete;
  StreamBase &operator=(StreamBase &&) = default;
  virtual ~StreamBase();

  static constexpr size_t defaultMaxReadSize = 4096;

  /// Read available data (up to `maxSize` bytes) from stream. Returns
  /// `std::nullopt` on EOF or closed handle (`close()`).
  ///
  /// Throws `UvcoException` on error.
  ///
  /// NOTE: Consider using `read(std::span<char>)` for better performance.
  ///
  /// NOTE: only one reader is allowed to be active at a time. If a read is
  /// started while another is still active, uvco will abort the process (in
  /// Debug mode), or ignore the first read (in Release mode).
  [[nodiscard]] Promise<std::optional<std::string>>
  read(size_t maxSize = defaultMaxReadSize);

  /// Read available data (up to `buffer.size()` bytes) from stream. Returns
  /// the number of bytes read, or 0 on EOF or closed handle (`close()`).
  ///
  /// Throws `UvcoException` on error.
  Promise<size_t> read(std::span<char> buffer);

  /// Write a buffer to the stream. A copy of `buf` is taken because it is
  /// undetermined when the actual write will occur. Await the result if the
  /// status is important; the write will be executed even without awaiting (as
  /// long as the process keeps running).
  ///
  /// NOTE: only one writer is allowed to be active at a time. If two writes
  /// are started simultaneously, the process will be aborted in Debug mode, or
  /// the first `write()` coroutine will not return in Release mode.
  [[nodiscard]] Promise<size_t> write(std::string buf);

  /// Shut down stream for writing. This is a half-close; the other side
  /// can still write. The result of `shutdown()` *must be `co_await`ed*.
  [[nodiscard]] Promise<void> shutdown();

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
  // Currently suspended readers/writers to be notified on close().
  std::optional<std::coroutine_handle<>> reader_;
  std::optional<std::coroutine_handle<>> writer_;

  struct ShutdownAwaiter_ {
    explicit ShutdownAwaiter_() {}
    static void onShutdown(uv_shutdown_t *req, uv_status status);

    bool await_ready();
    bool await_suspend(std::coroutine_handle<> handle);
    void await_resume();

    std::optional<std::coroutine_handle<>> handle_;
    std::optional<uv_status> status_;
  };

  struct InStreamAwaiter_ {
    explicit InStreamAwaiter_(StreamBase &stream, std::span<char> buffer)
        : stream_{stream}, buffer_{buffer} {}

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
    std::optional<std::coroutine_handle<>> handle_;
  };

  struct OutStreamAwaiter_ {
    OutStreamAwaiter_(StreamBase &stream, std::string_view buffer);

    [[nodiscard]] std::array<uv_buf_t, 1> prepare_buffers() const;

    bool await_ready();
    bool await_suspend(std::coroutine_handle<> handle);
    uv_status await_resume();

    static void onOutStreamWrite(uv_write_t *write, uv_status status);

    std::optional<std::coroutine_handle<>> handle_;
    std::optional<uv_status> status_;

    // State necessary for both immediate and delayed writing.
    std::string_view buffer_;
    uv_write_t write_{};
    StreamBase &stream_;
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

  static TtyStream tty(const Loop &loop, int fd);
  static TtyStream stdin(const Loop &loop) { return tty(loop, 0); }
  static TtyStream stdout(const Loop &loop) { return tty(loop, 1); }
  static TtyStream stderr(const Loop &loop) { return tty(loop, 2); }

private:
  explicit TtyStream(std::unique_ptr<uv_tty_t> stream)
      : StreamBase{std::move(stream)} {}
};

/// @}

} // namespace uvco
