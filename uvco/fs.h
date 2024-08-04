// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>
#include <uv/unix.h>

#include "uvco/bounded_queue.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

  /// Create a directory.
  static Promise<void> mkdir(const Loop &loop, std::string_view path,
                             int mode = 0755);
  /// Remove a directory. It must be empty.
  static Promise<void> rmdir(const Loop &loop, std::string_view path);
  /// Open a directory for reading.
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

class FsWatch {
public:
  /// Create aa new FsWatch instance. The path is the file or directory to
  /// watch.
  static FsWatch create(const Loop &loop, std::string_view path);

  /// Create a recursive watch. NOTE! This is only supported on macOS and
  /// Windows; see
  /// https://docs.libuv.org/en/v1.x/fs_event.html#c.uv_fs_event_start.
  static FsWatch createRecursive(const Loop &loop, std::string_view path);

  /// Destructor. If the watch is still active, it is stopped.
  ~FsWatch();

  /// An event in an observed file. If status is not 0, an error occurred; this
  /// doesn't mean however that no more events occur.
  struct FileEvent {
    FileEvent(std::string p, uv_fs_event e)
        : path{std::move(p)}, events{e}, status{} {}
    explicit FileEvent(uv_status s) : events{}, status{s} {}
    FileEvent(FileEvent &&) = default;
    FileEvent &operator=(FileEvent &&) = default;
    FileEvent(const FileEvent &) = default;
    FileEvent &operator=(const FileEvent &) = default;
    ~FileEvent() = default;

    std::string path;
    uv_fs_event events;
    uv_status status;
  };

  // Start watching the file or path. Events that occurred before calling
  // watch() are dropped.
  //
  // Note: watch() may be only called once at a time. However, after
  // stopWatch(), watch() can be called again.
  MultiPromise<FileEvent> watch();

  // In order to ensure a clean shutdown, call stopWatch() before dropping the
  // FsWatch, and consume all remaining events until the watch() generator
  // returns std::nullopt.
  //
  // You must supply the generator returned by watch() to ensure completion of
  // all events.
  Promise<void> stopWatch(MultiPromise<FileEvent> watcher);

  // Call this to retire the libuv handle.
  Promise<void> close();

private:
  FsWatch(const Loop &loop, std::string_view path, uv_fs_event_flags flags);
  MultiPromise<FileEvent> watch_();
  static void onFsWatcherEvent(uv_fs_event_t *handle, const char *path,
                               int events, uv_status status);

  struct FsWatchAwaiter_ {
    static constexpr unsigned defaultCapacity = 128;

    FsWatchAwaiter_();

    [[nodiscard]] bool await_ready() const;
    void await_suspend(std::coroutine_handle<> handle);
    bool await_resume();

    void schedule();
    void stop();
    void addError(uv_status status);
    void addEvent(FileEvent event);

    std::optional<std::coroutine_handle<>> handle_;
    BoundedQueue<FileEvent> events_;
    bool stopped_{};
  };

  uv_fs_event_t uv_handle_{};
};

/// @}

} // namespace uvco
