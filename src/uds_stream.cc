// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <cstddef>
#include <cstdio>
#include <fmt/core.h>
#include <string>
#include <uv.h>
#include <uv/version.h>

#include "exception.h"
#include "internal/internal_utils.h"
#include "uds.h"

#include <string_view>

namespace uvco {

namespace {

std::string getXname(uv_pipe_t *stream,
                     int (*getName)(const uv_pipe_t *, char *, size_t *)) {
  static constexpr size_t maxPath = 1024;
  std::string path;
  path.resize(maxPath);
  size_t pathSize = maxPath;
  const uv_status status = getName(stream, path.data(), &pathSize);
  if (status != 0) {
    throw UvcoException{status, "UnixStream::getXName failed"};
  }
  path.resize(pathSize);
  return path;
}

} // namespace

std::string UnixStream::getSockName() {
  return getXname((uv_pipe_t *)&stream(), uv_pipe_getsockname);
}

std::string UnixStream::getPeerName() {
  return getXname((uv_pipe_t *)&stream(), uv_pipe_getpeername);
}

} // namespace uvco
