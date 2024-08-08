// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <boost/assert.hpp>
#include <fmt/core.h>
#include <uv.h>
#include <uv/unix.h>

#include "uvco/close.h"
#include "uvco/exception.h"
#include "uvco/fs.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"

#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace uvco {

namespace {

class FileOpAwaiter_ {
  static void onFileOpDone(uv_fs_t *req) {
    auto *awaiter = getRequestData<FileOpAwaiter_>(req);
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
    setRequestData(&req_, this);
    handle_ = handle;
    return true;
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

Directory::Directory(Directory &&other) noexcept
    : loop_{other.loop_}, dir_{other.dir_} {
  other.loop_ = nullptr;
  other.dir_ = nullptr;
}

Directory &Directory::operator=(Directory &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  loop_ = other.loop_;
  dir_ = other.dir_;
  other.loop_ = nullptr;
  other.dir_ = nullptr;
  return *this;
}

Directory::~Directory() {
  if (dir_ != nullptr) {
    auto req = std::make_unique<uv_fs_t>();
    uv_fs_closedir(loop_, req.release(), dir_, nullptr);
    fmt::print(stderr, "Directory closed in dtor; this leaks memory. Please "
                       "use co_await close() instead\n");
  }
}

Promise<void> Directory::mkdir(const Loop &loop, std::string_view path,
                               int mode) {
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

Promise<Directory> Directory::open(const Loop &loop, std::string_view path) {
  FileOpAwaiter_ awaiter;

  uv_fs_opendir(loop.uvloop(), &awaiter.req(), path.data(),
                FileOpAwaiter_::uvCallback());

  co_await awaiter;

  co_return Directory{loop.uvloop(), (uv_dir_t *)awaiter.req().ptr};
}

Promise<std::vector<Directory::DirEnt>> Directory::read(unsigned count) {
  std::vector<DirEnt> result{count};
  const unsigned int size = co_await read(result);
  result.resize(size);
  co_return result;
}

Promise<unsigned int> Directory::read(std::span<DirEnt> buffer) {
  // dirents vector must be declared before awaiter, because awaiter will free
  // contents of dirents.
  std::vector<uv_dirent_t> dirents{buffer.size()};
  dir_->dirents = dirents.data();
  dir_->nentries = dirents.size();

  FileOpAwaiter_ awaiter;

  uv_fs_readdir(loop_, &awaiter.req(), dir_, FileOpAwaiter_::uvCallback());

  co_await awaiter;

  unsigned int nentries = awaiter.req().result;
  for (unsigned i = 0; i < nentries; ++i) {
    buffer[i].name = dirents[i].name;
    buffer[i].type = dirents[i].type;
  }
  co_return nentries;
}

MultiPromise<Directory::DirEnt> Directory::readAll(const Loop &loop,
                                                   std::string_view path) {
  FileOpAwaiter_ awaiter;
  uv_dirent_t dirent;
  const std::string pathCopy{path};

  uv_fs_scandir(loop.uvloop(), &awaiter.req(), pathCopy.c_str(), 0,
                FileOpAwaiter_::uvCallback());

  co_await awaiter;
  while (UV_EOF != uv_fs_scandir_next(&awaiter.req(), &dirent)) {
    co_yield DirEnt{dirent.name, dirent.type};
  }
}

Promise<void> Directory::close() {
  FileOpAwaiter_ awaiter;
  uv_fs_closedir(loop_, &awaiter.req(), dir_, FileOpAwaiter_::uvCallback());
  co_await awaiter;
  dir_ = nullptr;
  co_return;
}

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

FsWatch::FsWatch() : uv_handle_{std::make_unique<uv_fs_event_t>()} {}

FsWatch::~FsWatch() {
  if (!uv_handle_) {
    return;
  }
  BOOST_ASSERT_MSG(dataIsNull(uv_handle_.get()),
                   "don't drop an FsWatch while a watch() generator is "
                   "running (use stopWatch()!)");
  uv_fs_event_stop(uv_handle_.get());
}

Promise<FsWatch> FsWatch::create(const Loop &loop, std::string_view path) {
  return createWithFlag(loop, path, {});
}

Promise<FsWatch> FsWatch::createRecursive(const Loop &loop,
                                          std::string_view path) {
  return createWithFlag(loop, path, UV_FS_EVENT_RECURSIVE);
}

Promise<FsWatch> FsWatch::createWithFlag(const Loop &loop,
                                         std::string_view path,
                                         uv_fs_event_flags flags) {
  FsWatch fsWatch;
  uv_fs_event_t &uv_handle = *fsWatch.uv_handle_;
  const uv_status initStatus = uv_fs_event_init(loop.uvloop(), &uv_handle);
  if (initStatus != 0) {
    throw UvcoException{
        initStatus,
        "uv_fs_event_init returned error while initializing FsWatch"};
  }
  const auto startStatus =
      callWithNullTerminated<uv_status>(path, [&](std::string_view safePath) {
        return uv_fs_event_start(&uv_handle, onFsWatcherEvent, safePath.data(),
                                 flags);
      });
  if (startStatus != 0) {
    uv_fs_event_stop(&uv_handle);
    // This works with the current Loop::~Loop implementation.
    co_await closeHandle(&uv_handle);
    fsWatch.uv_handle_.reset();
    throw UvcoException{
        startStatus, "uv_fs_event_start returned error while starting FsWatch"};
  }
  co_return fsWatch;
}

MultiPromise<FsWatch::FileEvent> FsWatch::watch() {
  if (!dataIsNull(uv_handle_.get())) {
    throw UvcoException(UV_EBUSY, "FsWatch::watch() is already active!");
  }
  return watch_();
}

MultiPromise<FsWatch::FileEvent> FsWatch::watch_() {
  FsWatchAwaiter_ awaiter;
  setData(uv_handle_.get(), &awaiter);
  ZeroAtExit<void> zeroAtExit{&uv_handle_->data};

  while (co_await awaiter) {
    co_yield awaiter.events_.get();
  }
}

Promise<void> FsWatch::stopWatch(MultiPromise<FsWatch::FileEvent> watcher) {
  auto *awaiter = getData<FsWatchAwaiter_>(uv_handle_.get());
  awaiter->stop();
  co_await watcher;
}

Promise<void> FsWatch::close() {
  co_await closeHandle(uv_handle_.get());
  uv_handle_.reset();
}

void FsWatch::onFsWatcherEvent(uv_fs_event_t *handle, const char *path,
                               int events, uv_status status) {
  BOOST_ASSERT(events < 4);
  if (dataIsNull(handle)) {
    // No watcher generator set up yet.
    return;
  }
  auto *awaiter = getData<FsWatchAwaiter_>(handle);
  if (status == 0) {
    awaiter->addEvent(FileEvent{std::string{path}, (uv_fs_event)events});
  } else {
    awaiter->addError(status);
  }
  awaiter->schedule();
}

FsWatch::FsWatchAwaiter_::FsWatchAwaiter_() : events_{defaultCapacity} {}

bool FsWatch::FsWatchAwaiter_::await_ready() const {
  return stopped_ || !events_.empty();
}

void FsWatch::FsWatchAwaiter_::await_suspend(std::coroutine_handle<> handle) {
  BOOST_ASSERT(!handle_);
  handle_ = handle;
}

bool FsWatch::FsWatchAwaiter_::await_resume() {
  handle_.reset();
  BOOST_ASSERT(stopped_ || !events_.empty());
  return !stopped_;
}

void FsWatch::FsWatchAwaiter_::schedule() {
  if (handle_) {
    std::coroutine_handle<> handle = handle_.value();
    handle_.reset();
    Loop::enqueue(handle);
  }
}

void FsWatch::FsWatchAwaiter_::stop() {
  stopped_ = true;
  schedule();
}

void FsWatch::FsWatchAwaiter_::addError(uv_status status) {
  if (!events_.hasSpace()) {
    fmt::print(stderr, "uvco dropped FS event error {}", uv_strerror(status));
  }
  events_.put(FileEvent{status});
}

void FsWatch::FsWatchAwaiter_::addEvent(FileEvent event) {
  if (!events_.hasSpace()) {
    fmt::print(stderr, "uvco dropped FS event on {} due to queue overload\n",
               event.path);
    return;
  }
  events_.put(std::move(event));
}

} // namespace uvco
