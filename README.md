# uvco

C++20 standard library coroutines running on `libuv`.

Currently, more of an experiment - but it works for real! I am aiming for an
ergonomic, intuitive, asynchronous experience. In some parts, `uvco` implements
the bare minimum to still be joyful to use. Eventually, all of `libuv`'s
functionality should be available with low overhead.

Supported functionality:

* Name resolution (via `getaddrinfo`)
* UDP client/server, multicast, broadcast
* TCP client/server
* TTY (stdin/stdout)
* Anonymous pipes (operating-system-backed) and typed buffered channels (like Go's)
* Timer functionality (`sleep`, `tick`)

Promises (backed by coroutines) are run eagerly; you don't have to schedule or await them.
Many types of I/O-backed coroutines are run on a scheduler ([`src/scheduler.h`](src/scheduler.h)),
which you don't need to care much about. Other types always use eager execution, i.e.
an awaiting coroutine is resumed immediately from the callback of the I/O it is waiting on.
The scheduler is configurable and allows either immediate execution (on the callstack of a
callback) or deferred execution (after finishing all I/O, before the next I/O poll). Use
the doxygen documentation to find how to best use it.

Also, some types - like buffers received by sockets - use simplistic types like
strings. This will need to be generalized.

For specific examples, take a look at `src/event_loop.cc` (which sets up an
event loop and runs various asynchronous operations for manual
testing), or check out the unit tests in `test/`.

## Goal

Provide ergonomic asynchronous abstractions of all libuv functionality, at
satisfactory performance.

## Example

### Basic event loop set-up

```cpp
void run_loop(int disc) {
  // That's the default run mode for the scheduler:
  uvco::Scheduler scheduler{uvco::Scheduler::RunMode::Deferred};

  uv_loop_t loop;
  uv_loop_init(&loop);
  scheduler.setUpLoop(&loop);

  // A coroutine promise is run without having to wait on it: every co_await
  // triggers a callback subscription with libuv.
  // Create a "root" promise by calling a coroutine, e.g. one that
  // sets up a server.
  uvco::Promise<void> p = someAsynchronousFunction();

  // Run loop until everything is done.
  uv_run(&loop, UV_RUN_DEFAULT);

  assert(p.ready());

  uv_loop_close(&loop);
}
```

### HTTP 1.0 client

```cpp
// Using co_await in a function turns it into a coroutine. You can co_await all
// Promise and MultiPromise values; the right thing will happen.
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

// Manual setup: this will be part of uvco later.
void run_loop(int disc) {
  // As described in the first example.
  ...
}
```

### TCP Echo server

```cpp
Promise<void> echoReceived(TcpStream stream) {
  const AddressHandle peerAddress = stream.getPeerName();
  const std::string addressStr = peerAddress.toString();
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

  MultiPromise<TcpStream> clients = server.listen();

  while (true) {
    std::optional<TcpStream> client = co_await clients;
    if (!client)
      break;
    Promise<void> clientLoop = echoReceived(std::move(*client));
    clientLoops.push_back(clientLoop);
  }
}

int main(void) {
  // As described in the first example.
  ...
  Promise<void> server = echoTcpServer(&loop);
}

```

Some more examples can be found in the `test/` directory. Those test files
ending in `.exe.cc` are end-to-end binaries which also show how to set up
the event loop.

## Dependencies

* libuv (tested with 1.46, but > 1.0 probably works)
* libfmt (tested with 9.0)
* boost (boost-assert)
* gtest for unit testing (enabled by default).

## Building

Standard cmake build:

```bash
mkdir build && cd build
cmake ../CMakeLists.txt
make
```

In order to use it from your code, I suggest vendoring the entire source tree.
It is currently simple enough for that. This counts as static linking and falls
under the terms of the license (GNU LGPL 2.1).

## Testing

The code is tested by unit tests in `test/`; the coverage is currently > 90%.
Unit tests are especially helpful when built and run with `-DENABLE_ASAN=1
-DENABLE_COVERAGE=1`, detecting memory leaks and illegal accesses - the most
frequent bugs when writing asynchronous code.

Generally, run like this:

```shell
make && ctest --output-on-failure && make coverage
```

You can obtain coverage information using `make coverage` or `ninja coverage`.
The report is stored in `build/coverage/uvco.html`, and generated by
[gcovr](https://github.com/gcovr/gcovr), which should be installed.

## Documentation

*[Online documentation](https://borgac.net/~lbo/doc/uvco/)*

Documentation can be built using `doxygen`:

```shell
$ doxygen
```

and is delivered to the `doxygen/` directory.
