
#pragma once

#include <uv.h>

#include "stream.h"

namespace uvco {

/// @addtogroup pipe
/// @{

/// Creates a pipe pair. Data can be written to the second stream and read from
/// the first.
std::pair<StreamBase, StreamBase> pipe(uv_loop_t *);

} // namespace uvco
