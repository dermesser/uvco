#pragma once

#include "promise.h"

#include <coroutine>
#include <optional>
#include <string>
#include <uv.h>

namespace uvco {

class Stream {

public:
  using uv_status = int;

  // Takes ownership of stream.
  explicit Stream(uv_stream_t *stream) : stream_{stream} {}
  Stream(Stream &&) = default;
  Stream &operator=(Stream &&) = default;
  ~Stream();

  static Stream tty(uv_loop_t *loop, int fd);
  static Stream stdin(uv_loop_t *loop) { return tty(loop, 0); }
  static Stream stdout(uv_loop_t *loop) { return tty(loop, 1); }
  static Stream stderr(uv_loop_t *loop) { return tty(loop, 2); }

  Promise<std::optional<std::string>> read();

  Promise<uv_status> write(std::string buf);

  Promise<void> close(void (*uv_close_impl)(uv_handle_t *,
                                            uv_close_cb) = uv_close);

private:
  std::unique_ptr<uv_stream_t, UvHandleDeleter> stream_;

  struct InStreamAwaiter_ {
    explicit InStreamAwaiter_(Stream &stream) : stream_{stream}, slot_{} {}

    bool await_ready();
    bool await_suspend(std::coroutine_handle<> handle);
    std::optional<std::string> await_resume();

    void start_read();
    void stop_read();

    static void onInStreamRead(uv_stream_t *stream, ssize_t nread,
                               const uv_buf_t *buf);

    Stream &stream_;
    std::optional<std::optional<std::string>> slot_;
    std::optional<std::coroutine_handle<>> handle_;
  };

  struct OutStreamAwaiter_ {
    OutStreamAwaiter_(Stream &stream, std::string &&buffer);

    std::array<uv_buf_t, 1> prepare_buffers() const;

    bool await_ready();

    bool await_suspend(std::coroutine_handle<> handle);

    uv_status await_resume();

    static void onOutStreamWrite(uv_write_t *write, int status);

    Stream &stream_;
    std::optional<std::coroutine_handle<>> handle_;

    std::string buffer_;
    uv_write_t write_{};
    std::optional<uv_status> status_;
  };
};

}  // namespace uvco
