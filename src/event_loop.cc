#include <functional>
#include <uv.h>

#include <cassert>
#include <coroutine>
#include <fmt/format.h>
#include <optional>
#include <string_view>
#include <variant>

namespace {
void log(uv_loop_t *loop, std::string_view message) {
  static unsigned long count = 0;
  fmt::print("[{}] {}: {}\n", count++, uv_now(loop), message);
}

static void allocator(uv_handle_t * /*unused*/, size_t sugg, uv_buf_t *buf) {
  char *underlying = new char[sugg];
  buf->base = underlying;
  buf->len = sugg;
}

} // namespace

namespace uvco {

template <typename T> class Promise;

template <typename T> class Uvco : public std::coroutine_handle<Promise<T>> {
public:
  using promise_type = Promise<T>;

private:
};

template <typename T> class Promise {
public:
  using promise_type = Promise<T>;

  Promise() : handle_{}, slot_{std::make_shared<std::optional<T>>()} {}
  Promise(Promise &&) = default;
  Promise &operator=(const Promise &) = default;
  Promise &operator=(Promise &&) = default;
  Promise(const Promise<T> &other) = default;

  Promise<T> get_return_object() {
    std::coroutine_handle ch = Uvco<T>::from_promise(*this);
    handle_ = ch;
    return *this;
  }

  std::suspend_never initial_suspend() noexcept { return {}; }
  std::suspend_never final_suspend() noexcept { return {}; }
  void return_value(T &&value) { *slot_ = std::move(value); }
  void unhandled_exception() {}

  bool ready() { return slot_->has_value(); }
  T &result() { return **slot_; }

private:
  Promise(std::coroutine_handle<> h)
      : handle_{std::move(h)}, slot_{std::make_shared<std::optional<T>>()} {}

  std::optional<std::coroutine_handle<>> handle_;
  std::shared_ptr<std::optional<T>> slot_;
};

class Stdin {
  static constexpr const int fd = 0;

  struct StreamAwaiter {
    struct StreamAwaitState {
      std::coroutine_handle<> handle;
      std::shared_ptr<std::string> slot;
    };

    StreamAwaiter(uv_stream_t *stream)
        : stream_{stream}, slot_{std::make_shared<std::string>()} {}

    bool await_ready() const { return uv_is_readable(stream_) == 1234; }
    bool await_suspend(std::coroutine_handle<> handle) {
      auto state = new StreamAwaitState{handle, slot_};
      stream_->data = state;
      uv_read_start(stream_, allocator, onStreamRead);
      return true;
    }
    std::string await_resume() { return std::move(*slot_); }

    static void onStreamRead(uv_stream_t *stream, ssize_t nread,
                             const uv_buf_t *buf) {
      uv_read_stop(stream);

      auto *state = (StreamAwaitState *)stream->data;

      if (nread > 0) {
        std::string line{buf->base, static_cast<size_t>(nread)};
        *state->slot = std::move(line);
        log(stream->loop, fmt::format("Read {} bytes from stdin: {}", nread,
                                      std::string_view(buf->base, nread)));
      }
      log(stream->loop, "Resuming coroutine");

      state->handle.resume();

      delete state;
      delete[] buf->base;
    }

    uv_stream_t *stream_;
    std::shared_ptr<std::string> slot_;
  };

public:
  explicit Stdin(uv_loop_t *loop) : loop_{loop}, tty_{} {
    uv_tty_init(loop_, &tty_, fd, 0);
    tty_.data = this;
  }

  Promise<std::string> readLine() {
    StreamAwaiter awaiter{(uv_stream_t *)&tty_};

    std::string line = co_await awaiter;
    co_return line;
  };

private:
  uv_loop_t *loop_;
  uv_tty_t tty_;
};

class Data {
public:
  Data() = default;
};

void run_loop() {
  Data data;

  uv_loop_t loop;
  uv_loop_init(&loop);
  uv_loop_set_data(&loop, &data);

  Stdin s{&loop};
  Promise<std::string> p = s.readLine();

  log(&loop, "Before loop start");
  uv_run(&loop, UV_RUN_DEFAULT);
  log(&loop, "After loop end");

  assert(p.ready());
  fmt::print("obtained promised string: {}", p.result());

  uv_loop_close(&loop);
}

} // namespace uvco
