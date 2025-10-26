// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <uv.h>

#include "uvco/close.h"
#include "uvco/name_resolution.h"
#include "uvco/promise/promise.h"
#include "uvco/tcp_stream.h"

#include <boost/assert.hpp>
#include <fmt/core.h>
#include <fmt/format.h>
#include <sys/socket.h>

namespace uvco {

namespace {

void wrap_uv_tcp_close_reset(uv_tcp_t *handle, void (*cb)(uv_handle_t *)) {
  uv_tcp_close_reset(handle, cb);
}

} // namespace

Promise<void> TcpStream::closeReset() {
  co_await closeHandle((uv_tcp_t *)&stream(), wrap_uv_tcp_close_reset);
  destroyStream();
}

AddressHandle TcpStream::getSockName() const {
  struct sockaddr_storage addr{};
  int namelen = sizeof(addr);
  uv_tcp_getsockname((const uv_tcp_t *)underlying(), (struct sockaddr *)&addr,
                     &namelen);
  const AddressHandle address{(struct sockaddr *)&addr};
  return address;
}

AddressHandle TcpStream::getPeerName() const {
  struct sockaddr_storage addr{};
  int namelen = sizeof(addr);
  uv_tcp_getpeername((const uv_tcp_t *)underlying(), (struct sockaddr *)&addr,
                     &namelen);
  const AddressHandle address{(struct sockaddr *)&addr};
  return address;
};

void TcpStream::keepAlive(bool enable, unsigned int delay) {
  uv_tcp_keepalive((uv_tcp_t *)&stream(), static_cast<int>(enable), delay);
}

void TcpStream::noDelay(bool enable) {
  uv_tcp_nodelay((uv_tcp_t *)&stream(), static_cast<int>(enable));
}

} // namespace uvco
