// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include "promise.h"

#include <cstdint>
#include <memory>

namespace uvco {

// A promise that resolves after at least `millis` milliseconds.
Promise<void> wait(uv_loop_t *loop, uint64_t millis);

class TimerAwaiter;

class Ticker {
public:
  Ticker() = default;
  virtual ~Ticker() = default;
  virtual MultiPromise<uint64_t> ticker() = 0;
  virtual Promise<void> close() = 0;
};

// Yields a counter value, counting up from 0, at interval `millis`. If count is
// 0, the ticker will tick indefinitely.
//
// The returned ticker must be stoped using `co_await stop()` in order to avoid
// leaking resources. If `count` is not 0 (the ticker ends), it must be
// co_awaited until it returns UINT64_MAX, indicating proper clean-up of
// all resources.
std::unique_ptr<Ticker> tick(uv_loop_t *loop, uint64_t millis, uint64_t count);

} // namespace uvco
