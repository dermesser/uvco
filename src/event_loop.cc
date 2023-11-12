#include <chrono>
#include <uv.h>

#include "promise.h"
#include "name_resolution.h"
#include "stream.h"
#include "udp.h"
#include "close.h"

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

} // namespace

namespace uvco {

using std::coroutine_handle;

class TcpClient {
public:
  explicit TcpClient(uv_loop_t *loop, std::string target_host_address,
                     uint16_t target_host_port, int af_hint = AF_UNSPEC)
      : loop_{loop}, host_{std::move(target_host_address)}, af_hint_{af_hint},
        state_{State_::initialized}, port_{target_host_port} {}

  TcpClient(TcpClient &&other)
      : loop_{other.loop_}, host_{std::move(other.host_)},
        af_hint_{other.af_hint_}, state_{other.state_}, port_{other.port_} {
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

    AddressHandle ah =
        co_await resolver.gai(host_, fmt::format("{}", port_), af_hint_);
    fmt::print("Resolution OK: {}\n", ah.toString());

    uv_connect_t req;
    ConnectAwaiter_ connect{};
    req.data = &connect;

    state_ = State_::connecting;

    auto tcp = std::make_unique<uv_tcp_t>();

    uv_tcp_init(loop_, tcp.get());
    uv_tcp_connect(&req, tcp.get(), ah.sockaddr(), onConnect);

    co_await connect;

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
  int af_hint_;
  State_ state_;
  uint16_t port_;

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
  AddressHandle ah = co_await resolver.gai(name, "443");
  log(loop, fmt::format("After GAI: {}", ah.toString()));
  co_return;
}

// DANGER: due to the C++ standard definition, it is invalid to call a
// function returning Promise<T> with an argument accepted by a constructor of
// Promise<T>
// -- because then, the coroutine returns to itself!
Promise<int> fulfillWait(Promise<int> *p) {
  fmt::print("fulfill before await\n");
  int r = co_await *p;
  fmt::print("fulfill after await: {}\n", r);
  co_return r;
}

Promise<void> testHttpRequest(uv_loop_t *loop) {
  TcpClient client{loop, "borgac.net", 80, AF_INET};
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
  constexpr std::string_view buffer{"Hello back"};
  std::chrono::system_clock clock;
  const std::chrono::time_point zero = clock.now();

  Udp server{loop};
  co_await server.bind("::1", 9999, 0);

  std::chrono::time_point last = zero;
  MultiPromise<std::pair<std::string, AddressHandle>> packets =
      server.receiveMany();

  while (counter < 50) {
    /*
     * Can also be written as:
     *
     *   auto recvd = co_await server.receiveOneFrom();
     *   auto &buffer = recvd.first;
     *   auto &from = recvd.second;
     *
     *  With little/no performance impact.
     */
    auto recvd = co_await packets;
    if (!recvd)
      break;
    auto &buffer = recvd->first;
    auto &from = recvd->second;

    const std::chrono::time_point now = clock.now();
    const std::chrono::duration passed = now - last;
    last = now;
    const uint64_t passed_micros =
        std::chrono::duration_cast<std::chrono::microseconds>(passed).count();
    fmt::print("[{:03d} @ {:d}] Received >> {} << from {}\n", counter,
               passed_micros, buffer, from.toString());

    co_await server.send(std::span{buffer.begin(), buffer.end()}, from);

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

  for (uint32_t i = 0; i < max; ++i) {
    co_await client.send(std::span{msg}, {});
    auto response = co_await client.receiveOne();
    fmt::print("Response: {}\n", response);
  }

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

  Promise<void> p = testHttpRequest(&loop);

  /*
  Promise<void> p;
  if (disc == 0)
    p = udpServer(&loop);
  if (disc == 1)
    p = udpClient(&loop);
  */

  log(&loop, "Before loop start");
  uv_run(&loop, UV_RUN_DEFAULT);
  log(&loop, "After loop end");

  assert(p.ready());

  uv_loop_close(&loop);
}

} // namespace uvco
