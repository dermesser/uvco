# uvco

![GitHub Actions Workflow Status](https://img.shields.io/github/actions/workflow/status/dermesser/uvco/test.yml)

C++20 standard library coroutines running on `libuv`.

Currently, a bit of an experiment - but it works for real! I am aiming for an ergonomic, intuitive,
asynchronous experience. In some parts, `uvco` implements the bare minimum to still be joyful to
use. Eventually, all of `libuv`'s functionality should be available with low overhead.

Supported functionality:

* `Promise`s and generators (`MultiPromise`) as elementary coroutine abstractions
* Name resolution (via `getaddrinfo`)
* UDP client/server, multicast, broadcast
* TCP client/server
* TTY (stdin/stdout)
* Unix domain sockets (stream, client/server)
* Anonymous pipes (operating-system-backed) and typed buffered channels (like Go's)
* Timer functionality (`sleep`, `tick`)
* File functionality (`read`, `write`, `mkdir`, `unlink`, ...)
* A libcurl integration, allowing e.g. HTTP(S) downloads.
* A libpqxx integration, for asynchronous interaction with PostgreSQL databases.
* A threadpool for running synchronous or CPU-bound tasks
* A `SelectSet` for polling multiple promises at once
* Cancellation safety: dropped promises will cancel the underlying coroutine.

## Context

Promises (backed by coroutines) are run eagerly; you don't have to schedule or await them for the
underlying coroutine to run.

Where I/O or other activity causes a coroutine to be resumed, the coroutine will typically be run by
the scheduler, which you don't need to care about. Pending coroutines are resumed once per event
loop turn.

Some types - like buffers filled by sockets - use simple types like strings, which are easy to
handle but not super efficient. This may need to be generalized.

## Goal

uvco's goal is to provide ergonomic asynchronous abstractions of all libuv functionality, at
satisfactory performance.

## Examples

To run a coroutine, you need to set up an event loop. This is done by calling `uvco::runMain` with a
callable taking a single `const Loop&` argument and returns a `uvco::Promise<T>`. `runMain()` either
returns the resulting value after the event loop has finished, or throws an exception if a coroutine
threw one.

A `Promise<T>` is a coroutine promise, and can be awaited. It is the basic unit, and only access to
concurrency; there is no `Task` or such. Awaiting a promise will save the current execution state,
and resume it as soon as the promise is ready. A single coroutine is represented by a single
`Promise` object. Dropping the `Promise` will destroy (cancel) the coroutine. Some handles, like UDP
and stream sockets as well as timers have asynchronous close methods. It's best practice to `co_await` the
returned `Promise<void>`. If those handles are dropped, or inside a coroutine that has been
cancelled, the close operation will proceed asynchronously, and uvco will ensure that no memory is
leaked.

When in doubt, refer to the examples in `test/`; they are actively maintained.

### Basic event loop set-up

Return a promise from the main function run by `runMain()`. `runMain()` will return a
promised result, or throw an exception if a coroutine threw one. The event loop runs until
all callbacks are finished and all coroutines have been completed. Callbacks (by libuv)
trigger coroutine resumption from the event loop, which is defined in `src/run.cc`.

```cpp
#include <uvco/loop/loop.h>
#include <uvco/run.h>
#include <uvco/promise/promise.h>

using namespace uvco;

Promise<void> someAsynchronousFunction(const Loop& loop) {
  fmt::print("Hello from someAsynchronousFunction\n");
  co_await sleep(loop, 1000);
  fmt::print("Goodbye from someAsynchronousFunction\n");
}

void run_loop() {
  // A coroutine promise is run without having to wait on it: every co_await
  // triggers a callback subscription with libuv.
  // The `loop` mediates access to the event loop and is used by uvco's types.
  // Create a "root" promise by calling a coroutine, e.g. one that
  // sets up a server.
  runMain<void>([](const Loop& loop) -> Promise<void> {
    co_await someAsynchronousFunction(loop);
  });
}
```

### HTTP(S) download via libcurl

Here we download a single file. The `Curl` class is a wrapper around libcurl, and provides a
`download` method, which is a generator method returning a `MultiPromise`. The `MultiPromise` yields
`std::optional<std::string>`, which is a `std::nullopt` once the download has finished. An exception
is thrown if the download fails. To build the `curl-test`, which demonstrates this using a real
server, make sure to have `libcurl` and its headers installed. CMake should find it automatically.

Of course, more than one download can be triggered at once: `download()` MultiPromises are
independent.

```cpp
#include <uvco/integrations/curl/curl.h>
#include <uvco/loop/loop.h>
#include <uvco/promise/promise.h>
#include <uvco/run.h>

// and other includes ...

using namespace uvco;

Promise<void> testCurl(const Loop& loop) {
    Curl curl{loop};
    // API subject to change! (request must outlive download)
    auto request = curl.get("https://borgac.net/~lbo/doc/uvco");
    auto download = request.start();

    try {
      while (true) {
        auto result = co_await download;
        if (!result) {
          fmt::print("Downloaded file\n");
          break;
        }
        fmt::print("Received data: {}\n", *result);
      }
    } catch (const UvcoException &e) {
      fmt::print("Caught exception: {}\n", e.what());
    } catch (const CurlException& e) {
      fmt::print("Caught curl exception: {}\n", e.what());
    }

    co_await curl.close();
}

int main() {
  runMain<void>([](const Loop& loop) -> Promise<void> {
    return testCurl(loop);
  });
}
```

### HTTP 1.0 client

Build the project, and run the `test-http10` binary. It works like the following code:

```cpp
#include <uvco/loop/loop.h>
#include <uvco/promise/promise.h>
#include <uvco/tcp.h>
#include <uvco/tcp_stream.h>

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
// includes ...

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
    return echoTcpServer(loop);
  });
}

```

Some more examples can be found in the `test/` directory. Those test files
ending in `.exe.cc` are end-to-end binaries which also show how to set up
the event loop.

## Safety

### Lifetimes/references

Passing references and pointers into a coroutine (i.e. a function returning `[Multi]Promise<T>`) is
fine as long as the underlying value outlives the coroutine returning. Typically, this is done like
this:

```cpp
Promise<void> fun() {
    StackLocated value;
    Promise<void> promise = asynchronousFunction(&value);

    co_await promise;

    // At this point, the coroutine is guaranteed to have finished.

    co_return;
}
```

The temporary value is kept alive in the coroutine frame of `fun()`, which has
been allocated dynamically.

It's a different story when moving around promises: if the calling coroutine returns before the
awaited promise is finished, the result will be an illegal stack access. Don't do this :) Instead
make sure to e.g. use a `shared_ptr` instead of a reference, or a `std::string` instead of a
`std::string_view`.

```cpp
// BAD
Promise<void> coroutineBad(std::string_view sv) {
    // use-after-return once coroutine is scheduled to run
    fmt::print("Received string: {}\n", sv);
}

// GOOD
Promise<void> coroutineGood(std::string s) {
    // Coroutine owns its arguments, and this is safe.
    fmt::print("Received string: {}\n", s);
}

Promise<void> fun() {
    const std::string string = fmt::format("Hello {}", "world");
    return coroutine(string);
}
```

Another typical pattern that will not work is the following:

```cpp
Promise<void> returnsPromise(const Loop& loop) {
    auto in = TtyStream::stdin(loop);
    return in.read();
}
```

This will obviously cause a stack-use-after-return or other use-after-free error, depending on the
specific scenario.

Be extra careful of the following dangerous pattern around temporary values:

```cpp
Promise<void> takesStringView(std::string_view sv) {
    // sv is a reference to a temporary string, which will be destroyed
    // when the coroutine returns.
    fmt::print("Received string: {}\n", sv);
}

void run() {
    runMain<void>([](const Loop& loop) -> Promise<void> {
        // This is fine!
        co_await someAsyncFunction(fmt::format("Hello {}", "world");
        // But this isn't!
        Promise<void> p = takesStringView(fmt::format("Hello {}", "world"));
        co_await p;
    });
}
```

Some of these patterns are forbidden at compile time by `static_assert`s in the `Coroutine` class;
specifically, coroutines returning a `uvco::Promise` are not allowed to take parameters by rvalue;
this is almost always a bad idea and therefore we just ban it outright. As shown above, this ban
will not save you from stack-use-after-return in `std::string_view`, `std::span`, etc.

I may try to change the uvco types to prevent this pattern, but it's not easy to do so without
impairing the ease of use in other places. In general, it is safe to pass all arguments by value
into coroutines; especially as uvco evolves, the specific execution order of coroutines can change,
and something that worked previously may not work later. Imagine for example that a coroutine is not
immediately executed up to the first suspension point, but instead immediately scheduled for later
execution; this changes how coroutine arguments are accessed.

### Loop Lifetime

The `Loop` is singular, and outlives all coroutines running on it; therefore it's passed as `const
Loop&` to any coroutine needing to initiate I/O.

### Unfulfilled promises

If your application exits prematurely and receives an error about `EAGAIN`, and "unwrap called on unfulfilled promise",
it means that the event loop - either libuv's loop or uvco's - have decided that there's no more work to do, and thus
leave the loop:

```
Loop::~Loop(): uv_loop_close() failed; there were still resources on the loop: resource busy or locked
```

In a properly written application, this is the case once all the work has been done, i.e. no more open handles on the
libuv event loop, and no coroutines waiting to be resumed; for example, every unit test is required to behave like this
(see `test/`) and finish all operations before terminating. A typical case in which this occurs is working with
channels: they are implemented outside of libuv, and if no channel operation is currently pending, the uvco loop will
assume that no more work is to be done, and finsh the program's execution.

If there are open handles on the libuv event loop at this point, you will receive the exception described above.

## Exceptions

Exceptions are propagated through the coroutine stack. If a coroutine throws an exception, it will be thrown at the
point of the `co_await` that started the coroutine.

There are two difficulties:

1. `libuv` close() operations are asynchronous. In almost all cases, you should run `co_await obj.close()` to ensure
   correct freeing of resources. However, almost all uvco types emulate synchronous close in their destructor, so even
   if an exception is thrown or you forget to explicitly close an object, no resources nor memory will be leaked.
2. Exceptions are only rethrown from the `runMain()` call if the event loop has finished. If a single active libuv
   handle is present, this will not be the case, and the application will appear to hang. Therefore, prefer handling
   exceptions within your asynchronous code.

## Dependencies

* libuv (tested with 1.46, but > 1.0 probably works)
* libfmt (tested with 9.0)
* boost (boost-assert)
* gtest for unit testing (enabled by default).

## Building

Standard cmake build:

```bash
mkdir build && cd build
cmake -GNinja ../CMakeLists.txt
ninja
sudo ninja install
```

You can then use `uvco` in your own projects by linking against `uvco`. CMake packages are exported,
so you can use it in your cmake project as follows:

```cmake
# Your CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(your_project)

find_package(Uvco)

set(CMAKE_CXX_STANDARD 23)

add_executable(your_project main.cc)
target_link_libraries(your_project Uvco::uv-co-lib)
```

This will result in the following compiler invocations:

```shell
# Ninja syntax:
[1/2] /usr/bin/c++  \
    -std=gnu++23 -MD -MT \
    CMakeFiles/your_project.dir/main.cc.o \
    -MF CMakeFiles/your_project.dir/main.cc.o.d \
    -o CMakeFiles/your_project.dir/main.cc.o \
    -c /path/to/your/dev_tree/your_project/main.cc
[2/2] : && /usr/bin/c++   CMakeFiles/your_project.dir/main.cc.o \
    -o your_project  \
    /usr/local/lib/libuv-co-lib.a  /usr/local/lib/libuv-co-promise.a  /usr/local/lib/libuv-co-base.a  \
    -luv  -lpthread  -ldl  -lrt  -lfmt &&
```

Please let me know if this doesn't work for you (although I don't promise any help, I'm tired enough
of cmake already). Please note that - as usual - the order of libraries matters a great deal!
Link first against `libuv-co-lib`, then `-promise`, then `-base` so that all symbols can be resolved.

## Testing

The code is tested by unit tests in `test/`; the coverage is currently > 90%.
Unit tests are especially helpful when built and run with `-DENABLE_ASAN=1
-DENABLE_COVERAGE=1`, detecting memory leaks and illegal accesses - the most
frequent bugs when writing asynchronous code. I've found that Address Sanitizer
does not find all issues - `valgrind` is even more thorough.

For coverage information, you need `gcovr` or `grcov`. Typically these work
best when building with clang.

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
doxygen
```

and is delivered to the `doxygen/` directory.
