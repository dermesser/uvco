// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <boost/assert.hpp>
#include <fmt/core.h>
#include <span>
#include <uv.h>
#include <uv/unix.h>

#include "uvco/close.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"

#include <coroutine>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace uvco {

/// @addtogroup Stream
/// @{

/// A buffer pointer type suitable for vectored I/O. Can be constructed with the
/// same arguments as a std::span<const char> object, and be implicitly
/// converted to a std::span<const char> object.
class IoVec {
  uv_buf_t buf_;
public:
  template<class... Args>
  IoVec(Args&&... args) {
    std::span<const char> buf(std::forward<Args>(args)...);
    buf_.base = (char*)buf.data();
    buf_.len = buf.size();
  }
  constexpr IoVec& operator=( const IoVec& other ) = default;
  constexpr IoVec& operator=( const std::span<const char>& other ) {
    IoVec v(other);
    buf_ = v.buf_;
    return *this;
  }
  constexpr const char *data() { return (const char *)buf_.base; }
  constexpr size_t size() { return buf_.len; }
  operator std::span<const char> () const { return { buf_.base, buf_.len }; }
};

/// A plain stream, permitting reading, writing, and closing.
class StreamBase {
  struct ShutdownAwaiter_;
  struct InStreamAwaiter_;
  struct OutStreamAwaiter_;

public:
  template <typename Stream>
  explicit StreamBase(std::unique_ptr<Stream> stream)
      : stream_{(uv_stream_t *)stream.release()} {}
  StreamBase(const StreamBase &) = delete;
  StreamBase(StreamBase &&) = default;
  StreamBase &operator=(const StreamBase &) = delete;
  StreamBase &operator=(StreamBase &&) = default;

  /// Closes the stream if not already closed. It's always best to explicitly
  /// call `co_await stream.close()`, but as a backup, the destructor will close
  /// the stream if still open. To do so, it will schedule a callback on the
  /// libuv loop.
  virtual ~StreamBase();

  static constexpr size_t defaultMaxReadSize = 4080;

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
  /// Only one read() coroutine may be active at a time. The stream must outlive
  /// the coroutine, i.e. live until `co_await stream.read(...)` returns a
  /// result.
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
  [[nodiscard]] Promise<void> write(std::string buf);

  /// The same as `write(std::string)`, but takes a borrowed buffer. `buf` MUST
  /// absolutely stay valid until the promise resolves. This means: co_await
  /// this method and call it with a stored buffer (not a function return value,
  /// for example).
  [[nodiscard]] Promise<void> writeBorrowed(std::span<const char> buf);

  /// The same as `writeBorrowed(std::span<const char>)`, but takes multiple
  /// buffers, and writes them out in-order.
  ///
  /// NOTE: writeVectored may modify the `IoVec`s (not the buffer contents
  /// themselves) during writing, although it will restore the original values
  /// after completion. Do not use the `IoVec`s until the write is complete.
  [[nodiscard]] Promise<void> writeVectored(std::span<IoVec> bufs);

  /// Shut down stream for writing. This is a half-close; the other side
  /// can still write. The result of `shutdown()` *must be `co_await`ed*.
  [[nodiscard]] Promise<void> shutdown();

  /// Informs pending readers and writers of the close and causes them to return
  /// an empty optional.
  void close();

  /// Return the underlying UV stream object.
  [[nodiscard]] const uv_stream_t *underlying() const { return stream_.get(); }

  [[nodiscard]] uv_os_fd_t fd() const;

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
  std::coroutine_handle<> reader_;
  std::coroutine_handle<> writer_;
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
