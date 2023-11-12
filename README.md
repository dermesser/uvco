# uvco

C++ coroutines running on `libuv`.

Currently, more of an experiment - but it works for real!

Supported functionality:

* Name resolution (via `getaddrinfo`)
* UDP client/server
* TCP client
* TTY (stdin/stdout)

No scheduler is currently used: ready coroutines are run directly from libuv
callbacks. This works well, but a scheduler will probably be introduced at some
point.

## Goal

Provide asynchronous abstractions of all libuv functionality.

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

