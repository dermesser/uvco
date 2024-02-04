// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include "run.h"
#include "stream.h"

namespace uvco {

/// @addtogroup pipe
/// @{

/// Creates a pipe pair. Data can be written to the second stream and read from
/// the first.
std::pair<StreamBase, StreamBase> pipe(const Loop &loop);

} // namespace uvco
