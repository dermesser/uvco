
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <gtest/gtest.h>
#include <uv.h>

#include "test_util.h"
#include "uvco/exception.h"
#include "uvco/fs.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/promise.h"

#include <algorithm>
#include <cstddef>
#include <fcntl.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace uvco;

TEST(FsTest, OpenFile) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    auto file = co_await File::open(loop, "/dev/null", O_RDONLY);
    EXPECT_GT(file.file(), 0);
    co_await file.close();
  };

  run_loop(setup);
}

TEST(FsTest, FileNotFound) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    try {
      co_await File::open(loop, "/dev/does_not_exist", O_RDONLY);
      EXPECT_FALSE(true);
    } catch (const UvcoException &e) {
      EXPECT_EQ(e.status, UV_ENOENT);
    }
  };

  run_loop(setup);
}

TEST(FsTest, forgetClose) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    // At the moment this works without crashing asan.
    auto file = co_await File::open(loop, "/dev/null", O_RDONLY);
    EXPECT_GT(file.file(), 0);
  };

  run_loop(setup);
}

// Read some zeros.
TEST(FsTest, simpleRead) {
  static constexpr size_t bufSize = 32;

  auto setup = [](const Loop &loop) -> Promise<void> {
    auto file = co_await File::open(loop, "/dev/zero", O_RDONLY);
    EXPECT_GT(file.file(), 0);

    std::string buffer(bufSize, 'x');
    EXPECT_EQ(bufSize, buffer.size());

    size_t read = co_await file.read(buffer);

    EXPECT_EQ(bufSize, read);
    EXPECT_EQ(bufSize, buffer.size());
    EXPECT_TRUE(std::all_of(buffer.begin(), buffer.end(),
                            [](char c) { return c == 0; }));

    co_await file.close();
  };

  run_loop(setup);
}

TEST(FsTest, simpleReadWriteUnlink) {
  static constexpr std::string_view contents = "Hello World\n";
  static constexpr std::string_view fileName = "/tmp/_uvco_test_file";
  auto setup = [](const Loop &loop) -> Promise<void> {
    auto file = co_await File::open(loop, fileName, O_RDWR | O_CREAT);

    co_await file.write(contents);

    std::string buffer(64, '\0');

    const size_t bytesRead = co_await file.read(buffer, 0);

    EXPECT_EQ(contents.size(), bytesRead);
    EXPECT_EQ(contents, buffer);

    co_await file.close();

    co_await File::unlink(loop, fileName);
  };

  run_loop(setup);
}

TEST(FsTest, mkDirRmDir) {
  static constexpr std::string_view dirName = "/tmp/_uvco_test_dir";
  auto setup = [](const Loop &loop) -> Promise<void> {
    co_await Directory::mkdir(loop, dirName);

    try {
      co_await Directory::mkdir(loop, dirName);
    } catch (const UvcoException &e) {
      EXPECT_EQ(e.status, UV_EEXIST);
    }

    co_await Directory::rmdir(loop, dirName);
  };

  run_loop(setup);
}

TEST(FsTest, openDir) {
  static constexpr std::string_view dirName = "/tmp";
  auto setup = [](const Loop &loop) -> Promise<void> {
    auto dir = co_await Directory::open(loop, dirName);
    // Test move ctor.
    auto dir1 = std::move(dir);
    dir = std::move(dir1);

    auto entries = co_await dir.read();

    EXPECT_GT(entries.size(), 0);

    co_await dir.close();
  };

  run_loop(setup);
}

TEST(FsTest, scanDir) {
  static constexpr std::string_view dirName = "/tmp";
  auto setup = [](const Loop &loop) -> Promise<void> {
    auto entries = Directory::readAll(loop, dirName);
    unsigned count = 0;
    std::optional<Directory::DirEnt> entry;

    while ((entry = co_await entries).has_value()) {
      EXPECT_GT(entry->name.size(), 0);
      ++count;
    }
    EXPECT_GT(count, 0);
  };

  run_loop(setup);
}

} // namespace
