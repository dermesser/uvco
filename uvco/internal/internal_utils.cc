// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <boost/assert.hpp>
#include <fmt/core.h>
#include <uv.h>
#include <uv/unix.h>

#include "uvco/internal/internal_utils.h"

#include <algorithm>
#include <cstddef>

namespace uvco {

const bool TRACK_LIFETIMES = true;

/// libuv allocator.
void allocator(uv_handle_t * /*unused*/, size_t sugg, uv_buf_t *buf) {
  constexpr static size_t defaultSize = 4080;
  const size_t size = std::min(defaultSize, sugg);
  char *underlying = new char[size];
  buf->base = underlying;
  buf->len = size;
}

void freeUvBuf(const uv_buf_t *buf) {
  if (buf) {
    delete[] buf->base;
  }
}

void UvHandleDeleter::del(uv_handle_t *handle) {
  // Use dynamic type info tracked by libuv to delete the handle correctly.
  switch (handle->type) {
  case UV_ASYNC:
    delete (uv_async_t *)handle;
    break;
  case UV_CHECK:
    delete (uv_check_t *)handle;
    break;
  case UV_FS_EVENT:
    delete (uv_fs_event_t *)handle;
    break;
  case UV_FS_POLL:
    delete (uv_fs_poll_t *)handle;
    break;
  case UV_HANDLE:
    delete handle;
    break;
  case UV_IDLE:
    delete (uv_idle_t *)handle;
    break;
  case UV_POLL:
    delete (uv_poll_t *)handle;
    break;
  case UV_PREPARE:
    delete (uv_prepare_t *)handle;
    break;
  case UV_PROCESS:
    delete (uv_process_t *)handle;
    break;
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
  case UV_TIMER:
    delete (uv_timer_t *)handle;
    break;
  default:
    fmt::print("WARN: unhandled handle type {}\n", (int)handle->type);
    BOOST_ASSERT_MSG(false, "unhandled handle type");
  }
}

FlagGuard::FlagGuard(bool &flag) : flag_{flag} {
  BOOST_ASSERT(!flag_);
  flag = true;
}

FlagGuard::~FlagGuard() { flag_ = false; }

} // namespace uvco
