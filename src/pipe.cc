// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include "pipe.h"
#include "run.h"
#include "stream.h"

#include <array>
#include <memory>

namespace uvco {

std::pair<StreamBase, StreamBase> pipe(const Loop &loop) {
  std::array<uv_file, 2> fds{};
  uv_pipe(fds.data(), UV_NONBLOCK_PIPE, UV_NONBLOCK_PIPE);

  auto inp = std::make_unique<uv_pipe_t>();
  auto out = std::make_unique<uv_pipe_t>();

  uv_pipe_init(loop.uvloop(), inp.get(), 0);
  uv_pipe_init(loop.uvloop(), out.get(), 0);

  uv_pipe_open(inp.get(), fds[1]);
  uv_pipe_open(out.get(), fds[0]);

  return std::make_pair(StreamBase{std::move(out)}, StreamBase{std::move(inp)});
}

} // namespace uvco
