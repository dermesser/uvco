// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <fmt/core.h>
#include <string>
#include <uv.h>

#include <uv/version.h>

#include "stream.h"

namespace uvco {

/// @addtogroup UnixSockets
/// @{

/// A stream served by a Unix domain socket. In addition to the `StreamBase`
/// functionality, it provides getSockname() and getPeerName() methods.
///
/// A UnixStream cannot be created directly; use a `UnixStreamClient` or a
/// `UnixStreamServer` to create streams.
class UnixStream : public StreamBase {
public:
  UnixStream(const UnixStream &) = delete;
  UnixStream(UnixStream &&) = default;
  UnixStream &operator=(const UnixStream &) = delete;
  UnixStream &operator=(UnixStream &&) = default;
  ~UnixStream() override = default;

  using StreamBase::StreamBase;

  std::string getSockName();
  std::string getPeerName();
};

/// @}

} // namespace uvco
