// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <boost/assert.hpp>
#include <fcntl.h>
#include <functional>
#include <string_view>
#include <sys/types.h>
#include <utility>
#include <uv.h>

#include "exception.h"
#include "fs.h"
#include "loop/loop.h"
#include "promise/promise.h"

#include <coroutine>
#include <optional>
#include <uv/unix.h>

namespace uvco {

namespace {

class FileOpAwaiter_ {
  static void onFileOpDone(uv_fs_t *req) {
    auto *awaiter = static_cast<FileOpAwaiter_ *>(req->data);
    awaiter->result_ = req->result;
    if (awaiter->cb_) {
      awaiter->cb_(req);
    }
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

  /// Set the callback to be called when the operation is done, which is used to
  /// extract the result from the request.
  void setCallback(std::function<void(uv_fs_t *)> callback) {
    cb_ = std::move(callback);
  }

  [[nodiscard]] bool await_ready() const noexcept {
    return result_.has_value();
  }

  bool await_suspend(std::coroutine_handle<> handle) {
    BOOST_ASSERT(!result_);
    BOOST_ASSERT_MSG(!handle_, "FileOpAwaiter_ can only be awaited once");
    req_.data = this;
    handle_ = handle;
    return true;
    ;
  }

  /// The callback is guaranteed to have been called once the awaiting coroutine
  /// is resumed.
  void await_resume() {
    BOOST_ASSERT(result_);
    if (result_ && *result_ < 0) {
      throw UvcoException(*result_, "file operation failed");
    } else if (!result_) {
      throw UvcoException(UV_EAGAIN,
                          "file operation not yet finished (this is a bug)");
    }
  }

private:
  uv_fs_t req_ = {};
  std::function<void(uv_fs_t *)> cb_;
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

Promise<File> openFile(const Loop &loop, std::string_view path, int mode,
                       int flags) {
  FileOpAwaiter_ awaiter;
  uv_file file{};
  auto &req = awaiter.req();

  awaiter.setCallback(
      [&file](uv_fs_t *req) { file = static_cast<uv_file>(req->result); });

  uv_fs_open(loop.uvloop(), &req, path.data(), flags, mode,
             FileOpAwaiter_::uvCallback());

  co_await awaiter;
  co_return File{file};
}

Promise<void> closeFile(const Loop &loop, File file) {
  FileOpAwaiter_ awaiter;
  auto &req = awaiter.req();

  uv_fs_close(loop.uvloop(), &req, file.file(), FileOpAwaiter_::uvCallback());

  co_await awaiter;
}

} // namespace uvco
