// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include "promise/multipromise.h"
#include "promise/promise.h"

#include <cstdint>
#include <memory>

namespace uvco {

/// @addtogroup Timer
/// @{

/// A promise that resolves after at least `millis` milliseconds.
Promise<void> sleep(uv_loop_t *loop, uint64_t millis);

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

/// Yields a counter value, counting up from 0, at interval `millis`. If count
/// is 0, the ticker will tick indefinitely.
///
/// The returned ticker must be stoped using `co_await stop()` in order to avoid
/// leaking resources. If `count` is not 0 (so that only the specified number of
/// ticks is emitted), it must be co_awaited until it returns `std::nullopt`.
std::unique_ptr<Ticker> tick(uv_loop_t *loop, uint64_t millis, uint64_t count);

/// @}

} // namespace uvco
