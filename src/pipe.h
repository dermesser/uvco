// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "uvco/stream.h"

#include <utility>

namespace uvco {

class Loop;

/// @addtogroup pipe
/// @{

/// Creates a pipe pair. Data can be written to the second stream and read from
/// the first.
std::pair<StreamBase, StreamBase> pipe(const Loop &loop);

} // namespace uvco
