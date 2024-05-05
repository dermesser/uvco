// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <fcntl.h>
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
  [[nodiscard]] uv_file file() const { return file_; }

private:
  friend Promise<File> openFile(const Loop &loop, std::string_view path,
                                int mode, int flags);
  explicit File(uv_file file) : file_(file) {}

  uv_file file_;
};

/// Open a file asynchronously; flags and mode are optional and analogous to
/// `open(2)`.
Promise<File> openFile(const Loop &loop, std::string_view path,
                       int mode = O_RDWR, int flags = 0);

/// Close a file asynchronously.
Promise<void> closeFile(const Loop& loop, File file);

/// @}

} // namespace uvco
