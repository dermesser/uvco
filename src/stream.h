// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "promise.h"

#include <coroutine>
#include <optional>
#include <string>
#include <uv.h>

namespace uvco {

class StreamBase {
public:
  using uv_status = int;

  // Takes ownership of stream.
  explicit StreamBase(uv_stream_t *stream) : stream_{stream} {}
  StreamBase(const StreamBase &) = delete;
  StreamBase(StreamBase &&) = default;
  StreamBase &operator=(const StreamBase &) = delete;
  StreamBase &operator=(StreamBase &&) = default;
  virtual ~StreamBase();

  [[nodiscard]] Promise<std::optional<std::string>> read();

  [[nodiscard]] Promise<uv_status> write(std::string buf);

  // close() must be co_awaited!
  [[nodiscard]] Promise<void>
  close(void (*uv_close_impl)(uv_handle_t *, uv_close_cb) = uv_close);

  [[nodiscard]] const uv_stream_t *underlying() const { return stream_.get(); }

protected:
  std::unique_ptr<uv_stream_t, UvHandleDeleter> stream_;

private:
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

    static void onOutStreamWrite(uv_write_t *write, int status);

    StreamBase &stream_;
    std::optional<std::coroutine_handle<>> handle_;

    std::string buffer_;
    uv_write_t write_{};
    std::optional<uv_status> status_;
  };
};

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

} // namespace uvco
