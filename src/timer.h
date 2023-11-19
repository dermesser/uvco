// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include "promise.h"

#include <cstdint>

namespace uvco {

// A promise that resolves after at least `millis` milliseconds.
Promise<void> wait(uv_loop_t* loop, uint64_t millis);

} // namespace uvco
