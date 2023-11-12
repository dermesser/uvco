#include "internal_utils.h"

namespace uvco {

const bool TRACK_LIFETIMES = false;

void log(uv_loop_t *loop, std::string_view message) {
  static unsigned long count = 0;
  fmt::print("[{}] {}: {}\n", count++, uv_now(loop), message);
}
void allocator(uv_handle_t * /*unused*/, size_t sugg, uv_buf_t *buf) {
  constexpr static size_t defaultSize = 2048;
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
  case UV_TTY:
    delete (uv_tty_t *)handle;
    break;
  case UV_HANDLE:
    delete (uv_handle_t *)handle;
    break;
  default:
    fmt::print("WARN: unhandled handle type {}\n", handle->type);
    delete handle;
  }
}
} // namespace uvco
