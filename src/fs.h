// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <cstddef>
#include <fcntl.h>
#include <string>
#include <uv.h>
#include <uv/unix.h>

#include "loop/loop.h"
#include "promise/promise.h"

#include <string_view>

namespace uvco {

/// @addtogroup Filesystem
/// @{

/// A file descriptor.
class File {
public:
  /// Open a file asynchronously; flags and mode are optional and analogous to
  /// `open(2)`.
  static Promise<File> open(const Loop &loop, std::string_view path,
                            int mode = O_RDWR, int flags = 0);

  Promise<size_t> read(std::string& buffer, int64_t offset = -1);

  /// Access the libuv file handle.
  [[nodiscard]] uv_file file() const;

  /// Close a file asynchronously.
  Promise<void> close(const Loop &loop);

private:
  File(uv_loop_t* loop, uv_file file) : loop_{loop}, file_(file) {}

  uv_loop_t* loop_;
  uv_file file_;
};

/// @}

} // namespace uvco
