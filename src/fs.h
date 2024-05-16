// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <span>
#include <string>
#include <uv.h>
#include <uv/unix.h>

#include "loop/loop.h"
#include "promise/multipromise.h"
#include "promise/promise.h"

#include <string_view>
#include <vector>

namespace uvco {

/// @addtogroup Filesystem
/// @{

class Directory {
public:
  Directory(const Directory &) = default;
  Directory(Directory &&other) noexcept;
  Directory &operator=(const Directory &) = delete;
  Directory &operator=(Directory &&other) noexcept;
  ~Directory();

  struct DirEnt {
    std::string name;
    uv_dirent_type_t type;
  };

  static Promise<void> mkdir(const Loop &loop, std::string_view path,
                             int mode = 0755);
  static Promise<void> rmdir(const Loop &loop, std::string_view path);
  static Promise<Directory> open(const Loop &loop, std::string_view path);
  /// Read all directory entries of the given directory.
  static MultiPromise<DirEnt> readAll(const Loop &loop, std::string_view path);

  /// Read up to `count` directory entries.
  Promise<std::vector<DirEnt>> read(unsigned count = 64);

  /// Read up to `buffer.size()` directory entries into that buffer.
  Promise<unsigned int> read(std::span<DirEnt> buffer);

  /// Close the directory.
  Promise<void> close();

private:
  explicit Directory(uv_loop_t *loop, uv_dir_t *dir) : loop_{loop}, dir_{dir} {}

  uv_loop_t *loop_;
  uv_dir_t *dir_;
};

/// A file descriptor.
class File {
public:
  /// Open a file asynchronously; flags and mode are optional and analogous to
  /// `open(2)`.
  static Promise<File> open(const Loop &loop, std::string_view path,
                            int flags = 0, int mode = 0644);
  static Promise<void> unlink(const Loop &loop, std::string_view path);

  /// Read up to `buffer.size()` bytes into that buffer, starting at `offset`
  /// (if `offset >= 0`) or at the current file position.
  ///
  /// `buffer` is resized to the number of bytes read, if it could not be filled
  /// completely; The number of bytes read is also returned.
  ///
  /// TODO: generalize to any buffer type.
  Promise<size_t> read(std::string &buffer, int64_t offset = -1);

  /// Write contents of buffer to the underlying file at `offset`.
  ///
  /// TODO: generalize to any buffer type.
  Promise<size_t> write(std::string_view buffer, int64_t offset = -1);

  /// Access the libuv file handle.
  [[nodiscard]] uv_file file() const;

  /// Close a file asynchronously.
  Promise<void> close();

private:
  File(uv_loop_t *loop, uv_file file) : loop_{loop}, file_(file) {}

  uv_loop_t *loop_;
  uv_file file_;
};

/// @}

} // namespace uvco
