#include <chrono>
#include <uv.h>

#include "promise.h"

#include <algorithm>
#include <cassert>
#include <coroutine>
#include <fmt/format.h>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <typeinfo>

namespace {

void log(uv_loop_t *loop, std::string_view message) {
  static unsigned long count = 0;
  fmt::print("[{}] {}: {}\n", count++, uv_now(loop), message);
}

void allocator(uv_handle_t * /*unused*/, size_t sugg, uv_buf_t *buf) {
  constexpr static size_t defaultSize = 2048;
  const size_t size = std::min(defaultSize, sugg);
  char *underlying = new char[size];
  buf->base = underlying;
  buf->len = size;
}

void freeUvBuf(const uv_buf_t *buf) { delete[] buf->base; }

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
      fmt::print("WARN: unhandled handle type {}\n", handle->type);
      delete handle;
    }
  }
  template <typename Handle> void operator()(Handle *handle) {
    del((uv_handle_t *)handle);
  }
};

struct CloseAwaiter {
  bool await_ready() const { return closed_; }
  bool await_suspend(std::coroutine_handle<> handle) {
    handle_ = handle;
    return true;
  }
  void await_resume() {}

  std::optional<std::coroutine_handle<>> handle_;
  bool closed_ = false;
};

static void onCloseCallback(uv_handle_t *stream) {
  auto *awaiter = (CloseAwaiter *)stream->data;
  awaiter->closed_ = true;
  if (awaiter->handle_)
    awaiter->handle_->resume();
}

} // namespace

namespace uvco {

using std::coroutine_handle;

class Stream {

public:
  using uv_status = int;

  // Takes ownership of stream.
  explicit Stream(uv_stream_t *stream) : stream_{stream} {}
  Stream(Stream &&) = default;
  Stream &operator=(Stream &&) = default;
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

  Promise<void> close(void (*uv_close_impl)(uv_handle_t *,
                                            uv_close_cb) = uv_close) {
    // TODO: schedule closing operation on event loop?
    CloseAwaiter awaiter{};

    stream_->data = &awaiter;
    uv_close_impl((uv_handle_t *)stream_.get(), onCloseCallback);
    co_await awaiter;
    stream_.reset();
  }

private:
  std::unique_ptr<uv_stream_t, UvHandleDeleter> stream_;

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

      freeUvBuf(buf);
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
      // TODO: move before suspension point.
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

  Promise<struct addrinfo *> gai(std::string_view host, std::string_view port,
                                 int af_hint = AF_UNSPEC) {
    AddrinfoAwaiter_ awaiter;
    awaiter.req_.data = &awaiter;
    struct addrinfo hints {};
    hints.ai_family = af_hint;
    hints.ai_socktype = SOCK_STREAM;

    uv_getaddrinfo(loop_, &awaiter.req_, onAddrinfo, host.data(), port.data(),
                   &hints);
    // Npte: we rely on libuv not resuming before awaiting the result.
    struct addrinfo *result = co_await awaiter;

    int status = awaiter.status_.value();
    if (status != 0) {
      throw UvcoException{status, "getaddrinfo()"};
    }

    co_return result;
  }

private:
  uv_loop_t *loop_;

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
};

class Udp {
public:
  explicit Udp(uv_loop_t *loop)
      : loop_{loop}, udp_{std::make_unique<uv_udp_t>()} {
    uv_udp_init(loop, udp_.get());
  }
  Udp(Udp &&other) = default;
  Udp &operator=(Udp &&other) = default;
  Udp(const Udp &) = delete;
  Udp &operator=(const Udp &) = delete;
  ~Udp() { assert(!udp_); }

  Promise<void> bind(std::string_view address, uint16_t port,
                     unsigned int flag = 0) {
    Resolver r{loop_};
    int hint = 0;
    if (flag | UV_UDP_IPV6ONLY)
      hint = AF_INET6;
    struct addrinfo *ai =
        co_await r.gai(address, fmt::format("{}", port), hint);
    struct sockaddr *first = ai->ai_addr;

    int status = uv_udp_bind(udp_.get(), first, flag);
    uv_freeaddrinfo(ai);
    if (status != 0)
      throw UvcoException{status, "binding UDP socket"};
  }

  Promise<void> connect(std::string_view address, uint16_t port,
                        bool ipv6only = false) {
    Resolver r{loop_};
    int hint = ipv6only ? AF_INET6 : AF_UNSPEC;
    struct addrinfo *ai =
        co_await r.gai(address, fmt::format("{}", port), hint);
    struct sockaddr *first = ai->ai_addr;

    int status = uv_udp_connect(udp_.get(), first);
    uv_freeaddrinfo(ai);
    if (status != 0)
      throw UvcoException{status, "connecting UDP socket"};
    connected_ = true;
  }

  template <typename T>
  Promise<void> send(std::span<T> buffer, std::string_view address,
                     uint16_t port, bool ipv6only = false) {
    Resolver r{loop_};
    struct sockaddr *addr = nullptr;
    struct addrinfo *ai = nullptr;

    if (address.size() > 0) {
      int hint = ipv6only ? AF_INET6 : AF_UNSPEC;
      struct addrinfo *ai =
          co_await r.gai(address, fmt::format("{}", port), hint);
      addr = ai->ai_addr;
    }

    SendAwaiter_ sendAwaiter{};
    uv_udp_send_t req;
    req.data = &sendAwaiter;

    std::array<uv_buf_t, 1> bufs;
    bufs[0].base = &(*buffer.begin());
    bufs[0].len = buffer.size_bytes();

    int status =
        uv_udp_send(&req, udp_.get(), bufs.begin(), 1, addr, onSendDone);
    if (ai)
      uv_freeaddrinfo(ai);
    if (status != 0)
      throw UvcoException{status, "uv_udp_send() failed immediately"};

    int status_done = co_await sendAwaiter;
    if (status_done != 0)
      throw UvcoException{status_done, "uv_udp_send() failed while sending"};

    co_return;
  }

  Promise<std::string> receiveOne() {
    RecvAwaiter_ awaiter{};
    udp_->data = &awaiter;
    int status = uv_udp_recv_start(udp_.get(), allocator, onReceiveOne);
    if (status != 0)
      throw UvcoException(status, "uv_udp_recv_start()");

    std::string buffer = co_await awaiter;

    if (awaiter.nread_ && *awaiter.nread_ < 0)
      throw UvcoException(*awaiter.nread_, "uv UDP recv");

    udp_->data = nullptr;
    co_return buffer;
  }

  Promise<void> close() {
    CloseAwaiter awaiter{};
    udp_->data = &awaiter;
    uv_close((uv_handle_t *)udp_.get(), onCloseCallback);
    co_await awaiter;
    udp_.reset();
    connected_ = false;
  }

private:
  uv_loop_t *loop_;
  std::unique_ptr<uv_udp_t> udp_;
  bool connected_ = false;

  static void onReceiveOne(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                           const struct sockaddr *addr, unsigned int flags) {

    if (addr == nullptr && nread == 0) {
      if (handle->flags & UV_UDP_MMSG_FREE)
        freeUvBuf(buf);
      return;
    }

    uv_udp_recv_stop(handle);

    auto *awaiter = (RecvAwaiter_ *)handle->data;
    awaiter->nread_ = nread;
    awaiter->addr_ = *addr;
    if (nread > 0)
      awaiter->buffer_ = std::string{buf->base, static_cast<size_t>(nread)};

    if (0 == (handle->flags & UV_UDP_MMSG_CHUNK))
      freeUvBuf(buf);

    if (awaiter->handle_)
      awaiter->handle_->resume();
  }

  struct RecvAwaiter_ {
    bool await_ready() const { return buffer_.has_value(); }
    bool await_suspend(std::coroutine_handle<> h) {
      assert(!handle_);
      handle_ = h;
      return true;
    }
    std::string await_resume() {
      assert(buffer_);
      return std::move(*buffer_);
    }

    std::optional<std::string> buffer_;
    std::optional<std::coroutine_handle<>> handle_;
    std::optional<struct sockaddr> addr_;
    std::optional<int> nread_;
  };

  static void onSendDone(uv_udp_send_t *req, int status) {
    auto *awaiter = (SendAwaiter_ *)req->data;
    awaiter->status_ = status;
    if (awaiter->handle_)
      awaiter->handle_->resume();
  }

  struct SendAwaiter_ {
    bool await_ready() { return status_.has_value(); }
    bool await_suspend(std::coroutine_handle<> h) {
      assert(!handle_);
      handle_ = h;
      return true;
    }
    int await_resume() {
      assert(status_);
      return *status_;
    }

    std::optional<std::coroutine_handle<>> handle_;
    std::optional<int> status_;
  };
};

class TcpClient {
public:
  explicit TcpClient(uv_loop_t *loop, std::string target_host_address,
                     uint16_t target_host_port)
      : loop_{loop}, host_{std::move(target_host_address)},
        port_{target_host_port}, state_{State_::initialized} {}

  TcpClient(TcpClient &&other)
      : loop_{other.loop_}, host_{std::move(other.host_)}, port_{other.port_},
        state_{other.state_} {
    other.state_ = State_::invalid;
  }
  TcpClient(const TcpClient &) = delete;
  TcpClient &operator=(TcpClient &&other) {
    loop_ = other.loop_;
    host_ = std::move(other.host_);
    port_ = other.port_;
    state_ = other.state_;
    other.state_ = State_::invalid;
    return *this;
  }
  TcpClient &operator=(const TcpClient &) = delete;
  ~TcpClient() { assert(state_ != State_::connected); }

  Promise<void> connect() {
    Resolver resolver{loop_};

    state_ = State_::resolving;

    struct addrinfo *ai =
        co_await resolver.gai(host_, fmt::format("{}", port_));
    fmt::print("Resolution OK\n");
    struct sockaddr *addr = ai->ai_addr;

    uv_connect_t req;
    ConnectAwaiter_ connect{};
    req.data = &connect;

    state_ = State_::connecting;

    auto tcp = std::make_unique<uv_tcp_t>();

    uv_tcp_init(loop_, tcp.get());
    uv_tcp_connect(&req, tcp.get(), addr, onConnect);

    co_await connect;

    uv_freeaddrinfo(ai);

    if (connect.status_ < 0) {
      throw UvcoException(*connect.status_, "connect");
    }
    state_ = State_::connected;
    fmt::print("Connected successfully to {}:{}\n", host_, port_);
    connected_stream_ = Stream{(uv_stream_t *)(tcp.release())};
  }

  std::optional<Stream> &stream() { return connected_stream_; }

  Promise<void> close() {
    assert(connected_stream_);
    if (connected_stream_) {
      state_ = State_::closing;
      co_await connected_stream_->close(uv_tcp_close_reset_void);
      state_ = State_::closed;
      connected_stream_.reset();
    }
    co_return;
  }

private:
  enum class State_ {
    initialized = 0,
    resolving = 1,
    connecting = 2,
    connected = 3,
    failed = 4,
    closing = 5,
    closed = 6,

    invalid = 7,
  };

  uv_loop_t *loop_;

  std::string host_;
  uint16_t port_;
  State_ state_;

  // Maybe need call to uv_tcp_close_reset?
  std::optional<Stream> connected_stream_;

  static void uv_tcp_close_reset_void(uv_handle_t *handle, uv_close_cb cb) {
    uv_tcp_close_reset((uv_tcp_t *)handle, cb);
  }

  static void onConnect(uv_connect_t *req, int status) {
    fmt::print("onConnect from UV\n");
    auto *connect = static_cast<ConnectAwaiter_ *>(req->data);
    connect->onConnect(status);
  }

  struct ConnectAwaiter_ {
    bool await_ready() const { return status_.has_value(); }
    bool await_suspend(std::coroutine_handle<> h) {
      assert(!handle_);
      handle_ = h;
      return true;
    }
    int await_resume() {
      assert(status_);
      return *status_;
    }

    void onConnect(int status) {
      fmt::print("ConnectAwaiter::onConnect\n");
      status_ = status;
      if (handle_)
        handle_->resume();
    }

    std::optional<std::coroutine_handle<>> handle_ = {};
    std::optional<int> status_ = {};
  };
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
  co_await in.close();
  co_await out.close();
  co_return;
}

Promise<void> setupUppercasing(uv_loop_t *loop) {
  Stream in = Stream::stdin(loop);
  Stream out = Stream::stdout(loop);
  co_await uppercasing(std::move(in), std::move(out));
  co_return;
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

Promise<void> testHttpRequest(uv_loop_t *loop) {
  TcpClient client{loop, "ip6.me", 80};
  co_await client.connect();
  auto &stream = client.stream();
  assert(stream);

  co_await stream->write(
      fmt::format("HEAD / HTTP/1.0\r\nHost: borgac.net\r\n\r\n"));
  do {
    std::optional<std::string> chunk = co_await stream->read();
    if (chunk)
      fmt::print("Got chunk: >> {} <<\n", *chunk);
    else
      break;
  } while (true);
  co_await client.close();
}

Promise<void> udpServer(uv_loop_t *loop) {
  uint32_t counter = 0;
  std::chrono::system_clock clock;
  const std::chrono::time_point zero = clock.now();

  Udp server{loop};
  co_await server.bind("::1", 9999, 0);

  std::chrono::time_point last = zero;

  while (true) {
    std::string buffer = co_await server.receiveOne();
    const std::chrono::time_point now = clock.now();
    const std::chrono::duration passed = now - last;
    last = now;
    const uint64_t passed_micros =
        std::chrono::duration_cast<std::chrono::microseconds>(passed).count();
    fmt::print("[{:03d} @ {:d}] Received: {}\n", counter, passed_micros,
               buffer);

    ++counter;
  }
  co_await server.close();
  co_return;
}

Promise<void> udpClient(uv_loop_t *loop) {
  constexpr static uint32_t max = 16;
  std::string msg = "Hello there!";

  Udp client{loop};
  co_await client.connect("::1", 9999);

  for (uint32_t i = 0; i < max; ++i)
    co_await client.send(std::span{msg}, "", 0);

  co_await client.close();
}

void run_loop(int disc) {
  Data data;

  uv_loop_t loop;
  uv_loop_init(&loop);
  uv_loop_set_data(&loop, &data);

  // Promises are run even if they are not waited on or checked.

  // Promise<void> p = enumerateStdinLines(&loop);
  //  Promise<void> p = resolveName(&loop, "borgac.net");

  /*
  Fulfillable<int> f{};
  Promise<int> p2 = fulfillWait(&f.promise());
  f.fulfill(42);
  */
  // Promise<void> p = setupUppercasing(&loop);

  // Promise<void> p = testHttpRequest(&loop);

  Promise<void> p;
  if (disc == 0)
    p = udpServer(&loop);
  if (disc == 1)
    p = udpClient(&loop);

  log(&loop, "Before loop start");
  uv_run(&loop, UV_RUN_DEFAULT);
  log(&loop, "After loop end");

  assert(p.ready());

  uv_loop_close(&loop);
}

} // namespace uvco
