// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"

#include <cstdint>
#include <memory>

namespace uvco {

/// @addtogroup Timer
/// @{

/// A promise that resolves after at least `millis` milliseconds.
Promise<void> sleep(const Loop &loop, uint64_t millis);

/// A ticker produces events with a given periodicity. Use `tick()` to
/// create a `Ticker` instance.
class Ticker {
public:
  Ticker() = default;
  virtual ~Ticker() = default;
  /// A promise generating successive integers. If a count
  /// was given upon creation, the last tick after reaching 0 will yield
  /// std::nullopt.
  virtual MultiPromise<uint64_t> ticker() = 0;
  /// Immediately stop the ticker.
  virtual Promise<void> close() = 0;
};

/// Yields a counter value, counting up from 0, at interval `millis`. If `count`
/// is 0, the ticker will tick indefinitely.
///
/// If `count` is 0, the returned ticker must be stopped using `co_await stop()`
/// in order to avoid leaking resources.
///
/// If `count` is not 0 (so that only the specified number of
/// ticks is emitted), it must be co_awaited until it returns `std::nullopt`. If
/// not co_awaited until `std::nullopt` is returned, resources may be leaked. In
/// this case, `close()` needs not be called explicitly.
std::unique_ptr<Ticker> tick(const Loop &loop, uint64_t millis, uint64_t count);

/// @}

} // namespace uvco
