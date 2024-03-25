// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include "internal_utils.h"

#include <algorithm>
#include <boost/assert.hpp>
#include <uv.h>

namespace uvco {

const bool TRACK_LIFETIMES = false;

void log(uv_loop_t *loop, std::string_view message) {
  static unsigned long count = 0;
  fmt::print("[{}] {}: {}\n", count++, uv_now(loop), message);
}

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
  case UV_STREAM:
    delete (uv_stream_t *)handle;
    break;
  case UV_TCP:
    delete (uv_tcp_t *)handle;
    break;
  case UV_UDP:
    delete (uv_udp_t *)handle;
    break;
  case UV_NAMED_PIPE:
    delete (uv_pipe_t *)handle;
    break;
  case UV_TTY:
    delete (uv_tty_t *)handle;
    break;
  case UV_HANDLE:
    delete (uv_handle_t *)handle;
    break;
  case UV_TIMER:
    delete (uv_handle_t *)handle;
    break;
  case UV_PREPARE:
    delete (uv_prepare_t *)handle;
  case UV_CHECK:
    delete (uv_check_t *)handle;
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
