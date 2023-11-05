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

template <typename T> struct PromiseCore {
  std::optional<std::coroutine_handle<>> resume;
  std::optional<T> slot;
};

template <> struct PromiseCore<void> {
  std::optional<std::coroutine_handle<>> resume;
  bool ready = false;
};

template <typename T> class Promise {
  struct PromiseAwaiter_;
  using SharedCore_ = std::shared_ptr<PromiseCore<T>>;

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

  PromiseAwaiter_ operator co_await() { return PromiseAwaiter_{core_}; }

  bool ready() { return core_->slot.has_value(); }
  T result() { return std::move(*core_->slot); }

private:
  struct PromiseAwaiter_ {
    explicit PromiseAwaiter_(SharedCore_ core) : core_{std::move(core)} {}
    PromiseAwaiter_(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_(const PromiseAwaiter_ &) = delete;
    PromiseAwaiter_ &operator=(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_ &operator=(const PromiseAwaiter_ &) = delete;

    bool await_ready() const { return core_->slot.has_value(); }
    bool await_suspend(std::coroutine_handle<> handle) {
      if (core_->resume) {
        fmt::print("promise is already being waited on!\n");
        assert(false);
      }
      core_->resume = handle;
      return true;
    }
    T await_resume() { return std::move(core_->slot.value()); }

    SharedCore_ core_;
  };

  std::shared_ptr<PromiseCore<T>> core_;
};

template <> class Promise<void> {
  struct PromiseAwaiter_;
  using SharedCore_ = std::shared_ptr<PromiseCore<void>>;

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

  PromiseAwaiter_ operator co_await() { return PromiseAwaiter_{core_}; }

  bool ready() { return core_->ready; }
  void result() {}

private:
  struct PromiseAwaiter_ {
    explicit PromiseAwaiter_(SharedCore_ core) : core_{std::move(core)} {}
    PromiseAwaiter_(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_(const PromiseAwaiter_ &) = delete;
    PromiseAwaiter_ &operator=(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_ &operator=(const PromiseAwaiter_ &) = delete;

    bool await_ready() const { return core_->ready; }

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
  using uv_status = int;

  // Takes ownership of stream.
  explicit Stream(uv_stream_t *stream) : stream_{stream} {}
  Stream(Stream &&) = default;
  ~Stream() = default;

  static Stream tty(uv_loop_t *loop, int fd) {
    auto *tty = new uv_tty_t{};
    uv_tty_init(loop, tty, fd, 0);
    auto *stream = (uv_stream_t *)tty;
    return Stream(stream);
  }
  static Stream stdin(uv_loop_t *loop) { return tty(loop, 0); }
  static Stream stdout(uv_loop_t *loop) { return tty(loop, 1); }
  static Stream stderr(uv_loop_t *loop) { return tty(loop, 2); }

  Promise<std::optional<std::string>> read() {
    // This is a promise root function, i.e. origin of a promise.
    InStreamAwaiter awaiter{*this};
    std::optional<std::string> buf = co_await awaiter;
    co_return buf;
  }

  Promise<uv_status> write(std::string buf) {
    OutStreamAwaiter awaiter{*this, std::move(buf)};
    uv_status status = co_await awaiter;
    co_return status;
  }

private:
  std::unique_ptr<uv_stream_t> stream_;

  struct InStreamAwaiter {
    struct InStreamAwaitState {
      std::coroutine_handle<> handle;
      InStreamAwaiter *awaiter;
    };

    InStreamAwaiter(Stream &stream) : stream_{stream}, slot_{} {}

    bool await_ready() {
      int state = uv_is_readable(stream_.stream_.get());
      if (state == 1) {
        // Read available data and return immediately.
        start_read();
        stop_read();
      }
      return slot_.has_value();
    }
    bool await_suspend(std::coroutine_handle<> handle) {
      stream_.stream_->data = this;
      handle_ = handle;
      start_read();
      return true;
    }
    std::optional<std::string> await_resume() {
      assert(slot_);
      return std::move(*slot_);
    }

    void start_read() {
      uv_read_start(stream_.stream_.get(), allocator, onInStreamRead);
    }
    void stop_read() { uv_read_stop(stream_.stream_.get()); }

    static void onInStreamRead(uv_stream_t *stream, ssize_t nread,
                               const uv_buf_t *buf) {
      auto *awaiter = (InStreamAwaiter *)stream->data;
      awaiter->stop_read();

      if (nread >= 0) {
        std::string line{buf->base, static_cast<size_t>(nread)};
        awaiter->slot_ = std::move(line);
      } else {
        // Some error; assume EOF.
        awaiter->slot_ = std::optional<std::string>{};
      }

      awaiter->handle_.and_then([](auto h) -> std::optional<int> {
        h.resume();
        return {};
      });

      delete[] buf->base;
    }

    Stream &stream_;
    std::optional<std::optional<std::string>> slot_;
    std::optional<std::coroutine_handle<>> handle_;
  };

  struct OutStreamAwaiter {
    OutStreamAwaiter(Stream &stream, std::string &&buffer)
        : stream_{stream}, buffer_{std::move(buffer)} {}

    std::array<uv_buf_t, 1> prepare_buffers() const {
      std::array<uv_buf_t, 1> bufs;
      bufs[0].base = const_cast<char *>(buffer_.c_str());
      bufs[0].len = buffer_.size();
      return bufs;
    }

    bool await_ready() {
      // Attempt early write:
      auto bufs = prepare_buffers();
      int result =
          uv_try_write(stream_.stream_.get(), bufs.data(), bufs.size());
      if (result > 0)
        status_ = result;
      return result > 0;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
      write_.data = this;
      handle_ = handle;
      auto bufs = prepare_buffers();
      uv_write(&write_, stream_.stream_.get(), bufs.data(), bufs.size(),
               onOutStreamWrite);

      return true;
    }

    uv_status await_resume() {
      assert(status_);
      return *status_;
    }

    static void onOutStreamWrite(uv_write_t *write, int status) {
      auto *state = (OutStreamAwaiter *)write->data;
      assert(state->handle_);
      state->status_ = status;
      state->handle_->resume();
    }

    Stream &stream_;
    std::optional<std::coroutine_handle<>> handle_;

    std::string buffer_;
    uv_write_t write_;
    std::optional<uv_status> status_;
  };
};

class Data {
public:
  Data() = default;
};

Promise<void> uppercasing(Stream &in, Stream &out) {
  while (true) {
    auto maybeLine = co_await in.read();
    if (!maybeLine)
      break;
    auto line = maybeLine.value();
    std::ranges::transform(line, line.begin(),
                           [](char c) -> char { return std::toupper(c); });
    std::string to_output = fmt::format(">> {}", line);
    co_await out.write(std::move(to_output));
  }
  co_return;
}

void run_loop() {
  Data data;

  uv_loop_t loop;
  uv_loop_init(&loop);
  uv_loop_set_data(&loop, &data);

  Stream in = Stream::stdin(&loop);
  Stream out = Stream::stdout(&loop);
  Promise<void> p = uppercasing(in, out);

  log(&loop, "Before loop start");
  uv_run(&loop, UV_RUN_DEFAULT);
  log(&loop, "After loop end");

  assert(p.ready());

  uv_loop_close(&loop);
}

} // namespace uvco
