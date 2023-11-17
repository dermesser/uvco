// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <chrono>
#include <uv.h>

#include "close.h"
#include "name_resolution.h"
#include "promise.h"
#include "stream.h"
#include "tcp.h"
#include "udp.h"

#include <algorithm>
#include <cassert>
#include <coroutine>
#include <fmt/format.h>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <typeinfo>

namespace {} // namespace

namespace uvco {

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

Promise<void> uppercasing(TtyStream in, TtyStream out) {
  while (true) {
    auto maybeLine = co_await in.read();
    if (!maybeLine)
      break;
    auto line = maybeLine.value();
    std::transform(line.begin(), line.end(), line.begin(),
                   [](char c) { return std::toupper(c); });
    std::string to_output = fmt::format(">> {}", line);
    co_await out.write(std::move(to_output));
  }
  co_await in.close();
  co_await out.close();
  co_return;
}

Promise<void> setupUppercasing(uv_loop_t *loop) {
  TtyStream in = TtyStream::stdin(loop);
  TtyStream out = TtyStream::stdout(loop);
  co_await uppercasing(std::move(in), std::move(out));
  co_return;
}

MultiPromise<std::string> generateStdinLines(uv_loop_t *loop) {
  TtyStream in = TtyStream::stdin(loop);
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
  TcpClient client{loop, "borgac.net", 80, AF_INET6};
  TcpStream stream = co_await client.connect();

  co_await stream.write(
      fmt::format("HEAD / HTTP/1.0\r\nHost: borgac.net\r\n\r\n"));
  do {
    std::optional<std::string> chunk = co_await stream.read();
    if (chunk)
      fmt::print("Got chunk: >> {} <<\n", *chunk);
    else
      break;
  } while (true);
  co_await stream.closeReset();
}

Promise<void> udpServer(uv_loop_t *loop) {
  uint32_t counter = 0;
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
  constexpr static uint32_t max = 50;
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

Promise<void> testFunc(Promise<void> a) { co_await a; }

Promise<void> echoReceived(StreamBase stream) {
  struct sockaddr_storage addr {};
  int namelen = sizeof(addr);
  uv_tcp_getpeername((const uv_tcp_t *)stream.underlying(),
                     (struct sockaddr *)&addr, &namelen);
  const AddressHandle address{(struct sockaddr *)&addr};
  const std::string addressStr = address.toString();
  fmt::print("Received connection from [{}]\n", addressStr);

  while (true) {
    std::optional<std::string> p = co_await stream.read();
    if (!p) {
      break;
    }
    fmt::print("[{}] {}", addressStr, *p);
    co_await stream.write(std::move(*p));
  }
  co_await stream.close();
}

Promise<void> echoTcpServer(uv_loop_t *loop) {
  AddressHandle addr{"127.0.0.1", 8090};
  TcpServer server{loop, addr};
  std::vector<Promise<void>> clientLoops{};

  MultiPromise<StreamBase> clients = server.listen();

  while (true) {
    std::optional<StreamBase> client = co_await clients;
    if (!client)
      break;
    Promise<void> clientLoop = echoReceived(std::move(*client));
    // TODO: investigate if coroutine handles need to be destroyed?
    // Are the frames released automatically upon return?
    clientLoops.push_back(clientLoop);
  }
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
  Promise<void> p = setupUppercasing(&loop);

  Promise<void> p2 = testHttpRequest(&loop);

  // auto server = udpServer(&loop);
  // auto client = udpClient(&loop);

  // Promise<void> p = echoTcpServer(&loop);

  log(&loop, "Before loop start");
  uv_run(&loop, UV_RUN_DEFAULT);
  log(&loop, "After loop end");

  assert(p.ready());

  uv_loop_close(&loop);
}

} // namespace uvco
