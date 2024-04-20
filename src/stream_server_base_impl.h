// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include "stream_server_base.h"
#include "tcp_stream.h"
#include "uds_stream.h"

namespace uvco {

extern template class StreamServerBase<uv_tcp_t, TcpStream>;
extern template class StreamServerBase<uv_pipe_t, UnixStream>;

} // namespace uvco
