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
  std::optional<T> slot;
  std::optional<std::coroutine_handle<>> resume;
};

template <typename T> class Promise {
  struct PromiseAwaiter;

public:
  using promise_type = Promise<T>;

  Promise() : core_{std::make_shared<PromiseCore<T>>()} {}
  Promise(Promise &&) noexcept = default;
  Promise &operator=(const Promise &) = default;
  Promise &operator=(Promise &&) noexcept = default;
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

class Stdin {
  static constexpr const int fd = 0;

  struct StreamAwaiter {
    struct StreamAwaitState {
      std::coroutine_handle<> handle;
      StreamAwaiter *awaiter;
    };

    StreamAwaiter(uv_stream_t *stream) : stream_{stream}, slot_{} {}

    bool await_ready() const { return uv_is_readable(stream_) == 1234; }
    bool await_suspend(std::coroutine_handle<> handle) {
      auto *state = new StreamAwaitState{handle, this};
      stream_->data = state;
      uv_read_start(stream_, allocator, onStreamRead);
      return true;
    }
    std::optional<std::string> await_resume() {
      assert(slot_);
      return std::move(*slot_);
    }

    static void onStreamRead(uv_stream_t *stream, ssize_t nread,
                             const uv_buf_t *buf) {
      uv_read_stop(stream);

      auto *state = (StreamAwaitState *)stream->data;

      if (nread > 0) {
        std::string line{buf->base, static_cast<size_t>(nread)};
        state->awaiter->slot_ = std::move(line);
      } else {
        // Nested optional is not nice.
        state->awaiter->slot_ = std::optional<std::string>{};
        assert(state->awaiter->slot_);
      }

      state->handle.resume();

      delete state;
      delete[] buf->base;
    }

    uv_stream_t *stream_;
    std::optional<std::optional<std::string>> slot_;
  };

public:
  explicit Stdin(uv_loop_t *loop) : loop_{loop}, tty_{} {
    uv_tty_init(loop_, &tty_, fd, 0);
    tty_.data = this;
  }

  Promise<std::optional<std::string>> readLine() {
    // This is a promise root function, i.e. origin of a promise.
    StreamAwaiter awaiter{(uv_stream_t *)&tty_};
    std::optional<std::string> line = co_await awaiter;
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

Promise<Void> uppercasing(Stdin &s) {
  while (true) {
    auto maybeLine = co_await s.readLine();
    if (!maybeLine)
      break;
    auto line = maybeLine.value();
    std::ranges::transform(line, line.begin(),
                           [](char c) -> char { return std::toupper(c); });
    fmt::print(">> {}", line);
  }
  co_return {};
}

void run_loop() {
  Data data;

  uv_loop_t loop;
  uv_loop_init(&loop);
  uv_loop_set_data(&loop, &data);

  Stdin s{&loop};
  Promise<Void> p = uppercasing(s);

  log(&loop, "Before loop start");
  uv_run(&loop, UV_RUN_DEFAULT);
  log(&loop, "After loop end");

  assert(p.ready());

  uv_loop_close(&loop);
}

} // namespace uvco
