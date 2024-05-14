// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <functional>
#include <uv.h>

#include "loop/loop.h"
#include "promise/promise.h"

#include <optional>

namespace uvco {

/// Submit a function to be run on the libuv threadpool.
Promise<void> submitWork(const Loop &loop, std::function<void()> work);

/// Submit a function to be run on the libuv threadpool. The promise will return
/// the function's return value.
template <typename R>
  requires std::is_move_constructible_v<R>
Promise<R> submitWork(const Loop &loop, std::function<R()> work) {
  std::optional<R> result;
  // Erase return type and use generic submitWork().
  std::function<void()> agnosticWork = [&result, work]() { result = work(); };
  co_await submitWork(loop, agnosticWork);
  BOOST_ASSERT(result.has_value());
  co_return std::move(result.value());
}

} // namespace uvco
