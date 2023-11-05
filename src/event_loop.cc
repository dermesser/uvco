#include <uv.h>

#include <algorithm>
#include <cassert>
#include <coroutine>
#include <fmt/format.h>
#include <functional>
#include <optional>
#include <string_view>
#include <variant>

namespace {

void log(uv_loop_t *loop, std::string_view message) {
  static unsigned long count = 0;
  fmt::print("[{}] {}: {}\n", count++, uv_now(loop), message);
}

void allocator(uv_handle_t * /*unused*/, size_t sugg, uv_buf_t *buf) {
  char *underlying = new char[sugg];
  buf->base = underlying;
  buf->len = sugg;
}

} // namespace

namespace uvco {
using std::coroutine_handle;

struct Void {};

template <typename T> struct PromiseCore {
  std::optional<std::coroutine_handle<>> resume;
  std::optional<T> slot;
};

template <> struct PromiseCore<void> {
  std::optional<std::coroutine_handle<>> resume;
  bool ready = false;
};

template <typename T> class Promise {
  struct PromiseAwaiter;

public:
  using promise_type = Promise<T>;

  Promise() : core_{std::make_shared<PromiseCore<T>>()} {}
  Promise(Promise<T> &&) noexcept = default;
  Promise &operator=(const Promise<T> &) = default;
  Promise &operator=(Promise<T> &&) noexcept = default;
  Promise(const Promise<T> &other) = default;
  ~Promise() = default;

  Promise<T> get_return_object() { return *this; }

  std::suspend_never initial_suspend() noexcept { return {}; }
  std::suspend_never final_suspend() noexcept { return {}; }

  void return_value(T &&value) {
    core_->slot = std::move(value);
    if (core_->resume) {
      core_->resume->resume();
    }
  }
  void unhandled_exception() {}

  PromiseAwaiter operator co_await() { return PromiseAwaiter{core_}; }

  bool ready() { return core_->slot.has_value(); }
  T result() { return std::move(*core_->slot); }

private:
  struct PromiseAwaiter {
    bool await_ready() const { return false; }
    bool await_suspend(std::coroutine_handle<> handle) {
      if (core_->resume) {
        fmt::print("promise is already being waited on!\n");
        assert(false);
      }
      core_->resume = handle;
      return true;
    }
    T await_resume() { return std::move(core_->slot.value()); }

    std::shared_ptr<PromiseCore<T>> core_;
  };

  std::shared_ptr<PromiseCore<T>> core_;
};

template <> class Promise<void> {
  struct PromiseAwaiter;

public:
  using promise_type = Promise<void>;

  Promise() : core_{std::make_shared<PromiseCore<void>>()} {}
  Promise(Promise<void> &&) noexcept = default;
  Promise &operator=(const Promise<void> &) = default;
  Promise &operator=(Promise<void> &&) noexcept = default;
  Promise(const Promise<void> &other) = default;
  ~Promise() = default;

  Promise<void> get_return_object() { return *this; }

  std::suspend_never initial_suspend() noexcept { return {}; }
  std::suspend_never final_suspend() noexcept { return {}; }

  void return_void() {
    core_->ready = true;
    if (core_->resume) {
      core_->resume->resume();
    }
  }
  void unhandled_exception() {}

  PromiseAwaiter operator co_await() { return PromiseAwaiter{core_}; }

  bool ready() { return core_->ready; }
  void result() {}

private:
  struct PromiseAwaiter {
    bool await_ready() const { return false; }
    bool await_suspend(std::coroutine_handle<> handle) {
      if (core_->resume) {
        fmt::print("promise is already being waited on!\n");
        assert(false);
      }
      core_->resume = handle;
      return true;
    }
    void await_resume() {}

    std::shared_ptr<PromiseCore<void>> core_;
  };
  std::shared_ptr<PromiseCore<void>> core_;
};

class Stream {

public:
  // Takes ownership of stream.
  explicit Stream(uv_stream_t *stream) : stream_{stream} {}

  ~Stream() = default;

private:
  std::unique_ptr<uv_stream_t> stream_;
};

class InStream {

  struct InStreamAwaiter {
    struct InStreamAwaitState {
      std::coroutine_handle<> handle;
      InStreamAwaiter *awaiter;
    };

    InStreamAwaiter(InStream &stream) : stream_{stream}, slot_{} {}

    bool await_ready() const { return uv_is_readable(stream_.stream_) == 1234; }
    bool await_suspend(std::coroutine_handle<> handle) {
      auto *state = new InStreamAwaitState{handle, this};
      stream_.stream_->data = state;
      uv_read_start(stream_.stream_, allocator, onInStreamRead);
      return true;
    }
    std::optional<std::string> await_resume() {
      assert(slot_);
      return std::move(*slot_);
    }

    static void onInStreamRead(uv_stream_t *stream, ssize_t nread,
                               const uv_buf_t *buf) {
      uv_read_stop(stream);

      auto *state = (InStreamAwaitState *)stream->data;

      if (nread > 0) {
        std::string line{buf->base, static_cast<size_t>(nread)};
        state->awaiter->slot_ = std::move(line);
      } else {
        state->awaiter->slot_ = std::optional<std::string>{};
      }

      state->handle.resume();

      delete state;
      delete[] buf->base;
    }

    InStream &stream_;
    std::optional<std::optional<std::string>> slot_;
  };

public:
  // InStream takes ownership of the stream.
  explicit InStream(uv_stream_t *stream) : stream_{stream} {}

  ~InStream() { delete stream_; }

  static InStream stdin(uv_loop_t *loop) {
    static constexpr const int STDIN_FD = 0;
    auto *stdin = new uv_tty_t{};
    uv_tty_init(loop, stdin, STDIN_FD, 0);
    auto *stream = (uv_stream_t *)stdin;
    return InStream(stream);
  }

  Promise<std::optional<std::string>> readLine() {
    // This is a promise root function, i.e. origin of a promise.
    InStreamAwaiter awaiter{*this};
    std::optional<std::string> line = co_await awaiter;
    co_return line;
  };

private:
  // The stream is owned by this class.
  uv_stream_t *stream_;
};

class Data {
public:
  Data() = default;
};

Promise<void> uppercasing(InStream &s) {
  while (true) {
    auto maybeLine = co_await s.readLine();
    if (!maybeLine)
      break;
    auto line = maybeLine.value();
    std::ranges::transform(line, line.begin(),
                           [](char c) -> char { return std::toupper(c); });
    fmt::print(">> {}", line);
  }
  co_return;
}

void run_loop() {
  Data data;

  uv_loop_t loop;
  uv_loop_init(&loop);
  uv_loop_set_data(&loop, &data);

  InStream s = InStream::stdin(&loop);
  Promise<void> p = uppercasing(s);

  log(&loop, "Before loop start");
  uv_run(&loop, UV_RUN_DEFAULT);
  log(&loop, "After loop end");

  assert(p.ready());

  uv_loop_close(&loop);
}

} // namespace uvco
