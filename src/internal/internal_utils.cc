// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include "internal_utils.h"

#include <boost/assert.hpp>
#include <fmt/core.h>
#include <uv.h>
#include <uv/unix.h>

#include <algorithm>
#include <cstddef>

namespace uvco {

const bool TRACK_LIFETIMES = false;

/// libuv allocator.
void allocator(uv_handle_t * /*unused*/, size_t sugg, uv_buf_t *buf) {
  constexpr static size_t defaultSize = 4080;
  const size_t size = std::min(defaultSize, sugg);
  char *underlying = new char[size];
  buf->base = underlying;
  buf->len = size;
}

void freeUvBuf(const uv_buf_t *buf) {
  if (buf)
    delete[] buf->base;
}

void UvHandleDeleter::del(uv_handle_t *handle) {
  switch (handle->type) {
  case UV_TCP:
    delete (uv_tcp_t *)handle;
    break;
  case UV_NAMED_PIPE:
    delete (uv_pipe_t *)handle;
    break;
  case UV_TTY:
    delete (uv_tty_t *)handle;
    break;
  default:
    fmt::print("WARN: unhandled handle type {}\n", (int)handle->type);
    delete handle;
  }
}

FlagGuard::FlagGuard(bool &flag) : flag_{flag} {
  BOOST_ASSERT(!flag_);
  flag = true;
}

FlagGuard::~FlagGuard() { flag_ = false; }

} // namespace uvco
