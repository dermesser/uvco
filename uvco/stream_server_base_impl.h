// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include "uvco/stream_server_base.h"
#include "uvco/tcp_stream.h"
#include "uvco/uds_stream.h"

namespace uvco {

// Instantiated in stream_server_base_impl.cc to avoid keeping the templates in
// a header.
template class StreamServerBase<uv_tcp_t, TcpStream>;
template class StreamServerBase<uv_pipe_t, UnixStream>;

} // namespace uvco
