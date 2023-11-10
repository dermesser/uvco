#include <uv.h>

#include "promise.h"

#include <algorithm>
#include <cassert>
#include <coroutine>
#include <fmt/format.h>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <typeinfo>

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

struct UvHandleDeleter {
  void del(uv_handle_t *handle) {
    switch (handle->type) {
    case UV_STREAM:
      delete (uv_stream_t *)handle;
      break;
    case UV_TCP:
      delete (uv_tcp_t *)handle;
      break;
    case UV_UDP:
      delete (uv_udp_t *)handle;
      break;
    case UV_TTY:
      delete (uv_tty_t *)handle;
      break;
    case UV_HANDLE:
      delete (uv_handle_t *)handle;
      break;
    default:
      delete handle;
      fmt::print("WARN: unhandled handle type {}\n", handle->type);
    }
  }
  template <typename Handle> void operator()(Handle *handle) {
    del((uv_handle_t *)handle);
  }
};

} // namespace

namespace uvco {

using std::coroutine_handle;

class Stream {

public:
  using uv_status = int;

  // Takes ownership of stream.
  explicit Stream(uv_stream_t *stream) : stream_{stream} {}
  Stream(Stream &&) = default;
  ~Stream() {
    // close() MUST be called and awaited before dtor.
    assert(!stream_);
  }

  static Stream tty(uv_loop_t *loop, int fd) {
    auto *tty = new uv_tty_t{};
    int status = uv_tty_init(loop, tty, fd, 0);
    if (status != 0)
      throw UvcoException(
          fmt::format("opening TTY failed: {}", uv_err_name(status)));
    auto *stream = (uv_stream_t *)tty;
    return Stream(stream);
  }
  static Stream stdin(uv_loop_t *loop) { return tty(loop, 0); }
  static Stream stdout(uv_loop_t *loop) { return tty(loop, 1); }
  static Stream stderr(uv_loop_t *loop) { return tty(loop, 2); }

  Promise<std::optional<std::string>> read() {
    // This is a promise root function, i.e. origin of a promise.
    InStreamAwaiter_ awaiter{*this};
    std::optional<std::string> buf = co_await awaiter;
    co_return buf;
  }

  Promise<uv_status> write(std::string buf) {
    OutStreamAwaiter_ awaiter{*this, std::move(buf)};
    uv_status status = co_await awaiter;
    co_return status;
  }

  Promise<void> close() {
    // TODO: schedule closing operation on event loop?
    CloseAwaiter_ awaiter(*this);
    co_await awaiter;
    stream_.reset();
  }

private:
  std::unique_ptr<uv_stream_t, UvHandleDeleter> stream_;

  struct CloseAwaiter_ {
    explicit CloseAwaiter_(Stream &stream) : stream_{stream} {}

    bool await_ready() { return closed_; }
    bool await_suspend(std::coroutine_handle<> handle) {
      stream_.stream_->data = this;
      handle_ = handle;
      uv_close((uv_handle_t *)stream_.stream_.get(), onCloseCallback);
      return true;
    }
    void await_resume() {}

    static void onCloseCallback(uv_handle_t *stream) {
      CloseAwaiter_ *awaiter = (CloseAwaiter_ *)stream->data;
      awaiter->closed_ = true;
      assert(awaiter->handle_);
      awaiter->handle_->resume();
    }

    Stream &stream_;
    std::optional<std::coroutine_handle<>> handle_;
    bool closed_ = false;
  };

  struct InStreamAwaiter_ {
    explicit InStreamAwaiter_(Stream &stream) : stream_{stream}, slot_{} {}

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
      auto *awaiter = (InStreamAwaiter_ *)stream->data;
      awaiter->stop_read();

      if (nread >= 0) {
        std::string line{buf->base, static_cast<size_t>(nread)};
        awaiter->slot_ = std::move(line);
      } else {
        // Some error; assume EOF.
        awaiter->slot_ = std::optional<std::string>{};
      }

      if (awaiter->handle_) {
        awaiter->handle_->resume();
      }

      delete[] buf->base;
    }

    Stream &stream_;
    std::optional<std::optional<std::string>> slot_;
    std::optional<std::coroutine_handle<>> handle_;
  };

  struct OutStreamAwaiter_ {
    OutStreamAwaiter_(Stream &stream, std::string &&buffer)
        : stream_{stream}, buffer_{std::move(buffer)}, write_{} {}

    std::array<uv_buf_t, 1> prepare_buffers() const {
      std::array<uv_buf_t, 1> bufs{};
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
      auto *state = (OutStreamAwaiter_ *)write->data;
      state->status_ = status;
      assert(state->handle_);
      state->handle_->resume();
    }

    Stream &stream_;
    std::optional<std::coroutine_handle<>> handle_;

    std::string buffer_;
    uv_write_t write_{};
    std::optional<uv_status> status_;
  };
};

class Resolver {
  struct AddrinfoAwaiter_;

public:
  explicit Resolver(uv_loop_t *loop) : loop_{loop} {}

  Promise<struct addrinfo *> gai(std::string_view host, std::string_view port) {
    AddrinfoAwaiter_ awaiter;
    awaiter.req_.data = &awaiter;
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    uv_getaddrinfo(loop_, &awaiter.req_, onAddrinfo, host.data(), port.data(),
                   &hints);
    // Npte: we rely on libuv not resuming before awaiting the result.
    struct addrinfo *result = co_await awaiter;
    fmt::print("gai status: {}\n", awaiter.status_.value());
    co_return result;
  }

private:
  static void onAddrinfo(uv_getaddrinfo_t *req, int status,
                         struct addrinfo *result) {
    auto *awaiter = (AddrinfoAwaiter_ *)req->data;
    awaiter->addrinfo_ = result;
    awaiter->status_ = status;
    assert(awaiter->handle_);
    awaiter->handle_->resume();
  }

  struct AddrinfoAwaiter_ : public LifetimeTracker<AddrinfoAwaiter_> {
    AddrinfoAwaiter_() : req_{} {}
    bool await_ready() const { return false; }
    bool await_suspend(std::coroutine_handle<> handle) {
      handle_ = handle;
      return true;
    }

    struct addrinfo *await_resume() {
      assert(addrinfo_);
      return *addrinfo_;
    }

    uv_getaddrinfo_t req_;
    std::optional<struct addrinfo *> addrinfo_;
    std::optional<int> status_;
    std::optional<std::coroutine_handle<>> handle_;
  };

  uv_loop_t *loop_;
};

template <typename T> class Fulfillable {
public:
  Fulfillable() = default;

  void fulfill(T &&value) {
    assert(!promise_.ready());
    promise_.return_value(std::move(value));
  }

  Promise<T> &promise() { return promise_; }

private:
  Promise<T> promise_;
};

class TcpServer {

  // TODO...
public:
  // Takes ownership of tcp.
  explicit TcpServer(uv_tcp_t *tcp) : tcp_{tcp} {}
  explicit TcpServer(uv_loop_t *loop) : tcp_{std::make_unique<uv_tcp_t>()} {
    uv_tcp_init(loop, tcp_.get());
  }

  static constexpr const int IPV6_ONLY = UV_TCP_IPV6ONLY;

  void bind(struct sockaddr_in *addr, int flags = 0) {
    bind((struct sockaddr *)addr, flags);
  }
  void bind(struct sockaddr_in6 *addr, int flags = 0) {
    bind((struct sockaddr *)addr, flags);
  }

private:
  int bind(struct sockaddr *addr, int flags) {
    int result = uv_tcp_bind(tcp_.get(), addr, flags);
    if (result != 0) {
      throw UvcoException{result, "TcpServer::bind"};
    }
  }

  std::unique_ptr<uv_tcp_t> tcp_;
};

class Data {
public:
  Data() = default;
};

// Some demo and test functions.

Promise<void> uppercasing(Stream in, Stream out) {
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

Promise<void> setupUppercasing(uv_loop_t *loop) {
  Stream in = Stream::stdin(loop);
  Stream out = Stream::stdout(loop);
  Promise<void> p = uppercasing(std::move(in), std::move(out));
  return p;
}

MultiPromise<std::string> generateStdinLines(uv_loop_t *loop) {
  Stream in = Stream::stdin(loop);
  while (true) {
    std::optional<std::string> line = co_await in.read();
    if (!line)
      break;
    co_yield std::move(*line);
  }
  co_await in.close();
  co_return;
}

Promise<void> enumerateStdinLines(uv_loop_t *loop) {
  auto generator = generateStdinLines(loop);
  size_t count = 0;

  while (true) {
    ++count;
    std::optional<std::string> line = co_await generator;
    if (!line)
      break;
    fmt::print("{:3d} {}", count, *line);
  }
  co_return;
}

Promise<void> resolveName(uv_loop_t *loop, std::string_view name) {
  Resolver resolver{loop};
  log(loop, "Before GAI");
  struct addrinfo *ai = co_await resolver.gai(name, "443");
  struct sockaddr_in *sa = (struct sockaddr_in *)ai->ai_addr;
  unsigned char *addr_array = (unsigned char *)&sa->sin_addr.s_addr;
  std::span<unsigned char> a{addr_array, 4};
  log(loop, fmt::format("After GAI: {}.{}.{}.{}", a[0], a[1], a[2], a[3]));
  uv_freeaddrinfo(ai);
  co_return;
}

// DANGER: due to the C++ standard definition, it is invalid to call a function
// returning Promise<T> with an argument accepted by a constructor of Promise<T>
// -- because then, the coroutine returns to itself!
Promise<int> fulfillWait(Promise<int> *p) {
  fmt::print("fulfill before await\n");
  int r = co_await *p;
  fmt::print("fulfill after await: {}\n", r);
  co_return r;
}

void run_loop() {
  Data data;

  uv_loop_t loop;
  uv_loop_init(&loop);
  uv_loop_set_data(&loop, &data);

  // Promises are run even if they are not waited on or checked.

  Promise<void> p = enumerateStdinLines(&loop);
  Promise<void> p1 = resolveName(&loop, "borgac.net");

  Fulfillable<int> f{};
  Promise<int> p2 = fulfillWait(&f.promise());
  f.fulfill(42);
  //Promise<void> p3 = setupUppercasing(&loop);

  log(&loop, "Before loop start");
  uv_run(&loop, UV_RUN_DEFAULT);
  log(&loop, "After loop end");

  assert(p.ready());

  uv_loop_close(&loop);
}

} // namespace uvco
