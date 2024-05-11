// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <array>
#include <boost/assert.hpp>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <uv.h>

#include "exception.h"
#include "fs.h"
#include "internal/internal_utils.h"
#include "loop/loop.h"
#include "promise/promise.h"

#include <coroutine>
#include <optional>
#include <uv/unix.h>

namespace uvco {

namespace {

class FileOpAwaiter_ {
  static void onFileOpDone(uv_fs_t *req) {
    auto *awaiter =
        static_cast<FileOpAwaiter_ *>(uv_req_get_data((uv_req_t *)req));
    awaiter->result_ = req->result;
    awaiter->schedule();
  }

public:
  FileOpAwaiter_() = default;

  // File operation must be pinned in memory.
  FileOpAwaiter_(const FileOpAwaiter_ &) = delete;
  FileOpAwaiter_(FileOpAwaiter_ &&) = delete;
  FileOpAwaiter_ &operator=(const FileOpAwaiter_ &) = delete;
  FileOpAwaiter_ &operator=(FileOpAwaiter_ &&) = delete;

  ~FileOpAwaiter_() { uv_fs_req_cleanup(&req_); }

  /// Obtain the `uv_fs_t` struct to fill in before starting the operation.
  [[nodiscard]] uv_fs_t &req() { return req_; }
  [[nodiscard]] static uv_fs_cb uvCallback() { return onFileOpDone; }

  [[nodiscard]] bool await_ready() const noexcept {
    return result_.has_value();
  }

  bool await_suspend(std::coroutine_handle<> handle) {
    BOOST_ASSERT(!result_);
    BOOST_ASSERT_MSG(!handle_, "FileOpAwaiter_ can only be awaited once");
    uv_req_set_data((uv_req_t *)&req_, this);
    handle_ = handle;
    return true;
    ;
  }

  /// The callback is guaranteed to have been called once the awaiting coroutine
  /// is resumed.
  void await_resume() {
    BOOST_ASSERT(result_);
    if (result_ && result_.value() < 0) {
      throw UvcoException(static_cast<uv_status>(result_.value()),
                          "file operation failed");
    } else if (!result_) {
      throw UvcoException(UV_EAGAIN,
                          "file operation not yet finished (this is a bug)");
    }
  }

private:
  uv_fs_t req_ = {};
  std::optional<std::coroutine_handle<>> handle_;
  std::optional<ssize_t> result_ = std::nullopt;

  void schedule() {
    if (handle_) {
      const auto handle = handle_.value();
      handle_ = std::nullopt;
      Loop::enqueue(handle);
    }
  }
};

} // namespace

Promise<File> File::open(const Loop &loop, std::string_view path, int flags,
                         int mode) {
  FileOpAwaiter_ awaiter;

  uv_fs_open(loop.uvloop(), &awaiter.req(), path.data(), flags, mode,
             FileOpAwaiter_::uvCallback());

  co_await awaiter;

  const auto fileDesc = static_cast<uv_file>(awaiter.req().result);

  co_return File{loop.uvloop(), fileDesc};
}

Promise<void> File::unlink(const Loop &loop, std::string_view path) {
  FileOpAwaiter_ awaiter;

  uv_fs_unlink(loop.uvloop(), &awaiter.req(), path.data(),
               FileOpAwaiter_::uvCallback());

  co_await awaiter;
  co_return;
}

Promise<void> Directory::mkdir(const Loop &loop, std::string_view path, int mode) {
  FileOpAwaiter_ awaiter;

  uv_fs_mkdir(loop.uvloop(), &awaiter.req(), path.data(), mode,
              FileOpAwaiter_::uvCallback());

  co_await awaiter;
  co_return;
}

Promise<void> Directory::rmdir(const Loop &loop, std::string_view path) {
  FileOpAwaiter_ awaiter;

  uv_fs_rmdir(loop.uvloop(), &awaiter.req(), path.data(),
              FileOpAwaiter_::uvCallback());

  co_await awaiter;
  co_return;
}

Promise<size_t> File::read(std::string &buffer, int64_t offset) {
  FileOpAwaiter_ awaiter;
  size_t result = 0;

  std::array<uv_buf_t, 1> bufs{};
  bufs[0].base = buffer.data();
  bufs[0].len = buffer.length();

  uv_fs_read(loop_, &awaiter.req(), file(), bufs.data(), 1, offset,
             FileOpAwaiter_::uvCallback());

  co_await awaiter;

  result = awaiter.req().result;
  buffer.resize(result);
  co_return result;
}

Promise<size_t> File::write(std::string_view buffer, int64_t offset) {
  FileOpAwaiter_ awaiter;
  size_t result = 0;

  std::array<uv_buf_t, 1> bufs{};
  // Unfortunately necessary: const_cast
  bufs[0].base = const_cast<char *>(buffer.data());
  bufs[0].len = buffer.length();

  uv_fs_write(loop_, &awaiter.req(), file(), bufs.data(), 1, offset,
              FileOpAwaiter_::uvCallback());

  co_await awaiter;

  result = awaiter.req().result;
  co_return result;
}

uv_file File::file() const {
  BOOST_ASSERT(file_ >= 0);
  return file_;
}

Promise<void> File::close() {
  FileOpAwaiter_ awaiter;
  auto &req = awaiter.req();

  uv_fs_close(loop_, &req, file(), FileOpAwaiter_::uvCallback());

  file_ = -1;

  co_await awaiter;
}

} // namespace uvco
