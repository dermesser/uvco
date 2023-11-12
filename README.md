# uvco

C++ coroutines running on `libuv`.

Currently, more of an experiment - but it works for real! I am aiming for an
ergonomic, intuitive, asynchronous experience.

Supported functionality:

* Name resolution (via `getaddrinfo`)
* UDP client/server
* TCP client
* TTY (stdin/stdout)

No scheduler is currently used: ready coroutines are run directly from libuv
callbacks. This works well, but a scheduler will probably be introduced at some
point.

Also, some types - like buffers received by sockets - use simplistic types like
strings. This will need to be generalized.

## Goal

Provide ergonomic asynchronous abstractions of all libuv functionality.

## Example

```c++
// Using co_await in a function turns it into a coroutine. You can co_await all
// Promise and MultiPromise values; the right thing will happen.
Promise<void> testHttpRequest(uv_loop_t *loop) {
uvco::TcpClient client{loop, "borgac.net", 80, AF_INET};
  co_await client.connect();

  std::optional<uvco::Stream> &stream = client.stream();
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

// Manual setup: this will be part of uvco later.
void run_loop(int disc) {
  uv_loop_t loop;
  uv_loop_init(&loop);

  // A coroutine promise is run without having to wait on it: every co_await
  // triggers a callback subscription with libuv.
  uvco::Promise<void> p = testHttpRequest(&loop);

  // Runs until everything is done.
  uv_run(&loop, UV_RUN_DEFAULT);

  assert(p.ready());

  uv_loop_close(&loop);
}
```

## Dependencies

* libuv (tested with 1.46, but > 1.0 probably works)
* libfmt (tested with 9.0)

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

