
#include "exception.h"
#include "fs.h"
#include "loop/loop.h"
#include "promise/promise.h"
#include "test_util.h"

#include <algorithm>
#include <cstddef>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <string>
#include <uv.h>

namespace {

using namespace uvco;

TEST(FsTest, OpenFile) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    auto file = co_await File::open(loop, "/dev/null", O_RDONLY);
    EXPECT_GT(file.file(), 0);
    co_await file.close(loop);
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

    co_await file.close(loop);
  };

  run_loop(setup);
}

} // namespace
