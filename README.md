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

## Context

Promises (backed by coroutines) are run eagerly; you don't have to schedule or await them for the
underlying coroutine to run.

Where I/O causes a coroutine to be resumed, the coroutine will typically be run
by the scheduler ([`src/scheduler.h`](src/scheduler.h)), which you don't need
to care about. Other types of events, such as a generator yielding or a
coroutine returning, will cause awaiting code to be resumed directly on the
call stack.

However, as a user you shouldn't have to care about this. While you can set the
scheduling mode for I/O events in `uvco::runMain()` (`Deferred` vs.
`Immediate`), the externally visible behavior should be the same, and code will
work in both modes. If it doesn't: that's a bug in uvco.

Some types - like buffers received by sockets - use simple types like strings,
which are easy to handle but not super efficient. This may need to be
generalized.

## Goal

Provide ergonomic asynchronous abstractions of all libuv functionality, at
satisfactory performance.

## Example

To run a coroutine, you need to set up an event loop. This is done by calling
`uvco::runMain` with a lambda that returns a `uvco::Promise<T>`. `runMain()` either
returns the resulting value after the event loop has finished, or throws an exception if
a coroutine threw one.

A `Promise<T>` is a coroutine promise, and can be awaited. It is the basic unit, and only
access to concurrency; there is no `Task` or such. Awaiting a promise will save the current
execution state, and resume it as soon as the promise is ready. Currently, promises cannot
be cancelled.

When in doubt, refer to the examples in `test/`; they are actively maintained and keep the
current state of the examples below.

### Basic event loop set-up

Return a promise from the main function run by `runMain()`. `runMain()` will return a
promised result, or throw an exception if a coroutine threw one. The event loop runs until
all callbacks are finished and all coroutines have been completed. Callbacks (by libuv)
trigger coroutine resumption from the event loop, which is defined in `src/run.cc`.

```cpp
void run_loop() {
  // A coroutine promise is run without having to wait on it: every co_await
  // triggers a callback subscription with libuv.
  // The `loop` mediates access to the event loop and is used by uvco's types.
  // Create a "root" promise by calling a coroutine, e.g. one that
  // sets up a server.
  uvco::runMain<void>([](const Loop& loop) -> uvco::Promise<void> {
    co_await someAsynchronousFunction(loop);
  });
}
```

### HTTP 1.0 client

Build the project, and run the `test-http10` binary. It works like the following code:

```cpp
using namespace uvco;

// Using co_await in a function turns it into a coroutine. You can co_await all
// Promise and MultiPromise values; the right thing will happen.
Promise<void> testHttpRequest(const Loop& loop) {
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
void run_loop() {
  // As described in the first example.
  runMain<void>([](const Loop& loop) -> Promise<void> {
    Promise<void> p = testHttpRequest(loop);
    co_await p;
  });
}
```

### TCP Echo server

```cpp
using namespace uvco;

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

Promise<void> echoTcpServer(const Loop& loop) {
  AddressHandle addr{"127.0.0.1", 8090};
  TcpServer server{loop, addr};
  std::vector<Promise<void>> clientLoops{};

  MultiPromise<TcpStream> clients = server.listen();

  while (true) {
    std::optional<TcpStream> client = co_await clients;
    if (!client) {
      break;
    }
    Promise<void> clientLoop = echoReceived(std::move(*client));
    clientLoops.push_back(clientLoop);
  }
}

int main(void) {
  // It also works with a plain function: awaiting a promise is not necessary
  // (but more intuitive).
  runMain<void>([](const Loop& loop) -> Promise<void> {
    Promise<void> server = echoTcpServer(loop);
    return server;
  });
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

For coverage information, you need `gcovr`.

Generally, run it like this:

```shell
# or ninja instead of make:
make && ctest --output-on-failure && make coverage
# then open build/coverage/uvco.html
```

You can obtain coverage information using `make coverage` or `ninja coverage`.
The report is stored in `build/coverage/uvco.html`, and generated by
[gcovr](https://github.com/gcovr/gcovr), which should be installed. Alternatively,
use `make grcov` in order to use the [`grcov`](https://github.com/mozilla/grcov) tool.
The coverage html is in `build/coverage/uvco.html` respectively `build/grcov/html/index.html`.

For coverage, I recommend using `clang++` (`-DCMAKE_CXX_COMPILER=clang++`) because
`g++` does not take into account lines within coroutines - which is kind of pointless
in a coroutine library. The `gcovr` invocation defined in `CMakeLists.txt` handles
both cases, invoking `llvm-cov` when compiling with `clang++`.

## Documentation

*[Online documentation](https://borgac.net/~lbo/doc/uvco/)*

Documentation can be built using `doxygen`:

```shell
$ doxygen
```

and is delivered to the `doxygen/` directory.
