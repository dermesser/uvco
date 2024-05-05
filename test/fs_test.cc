
#include "exception.h"
#include "fs.h"
#include "loop/loop.h"
#include "promise/promise.h"
#include "test_util.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <uv.h>

namespace {

using namespace uvco;

TEST(FsTest, OpenFile) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    auto file = co_await openFile(loop, "/dev/null", O_RDONLY);
    EXPECT_GT(file.file(), 0);
    fmt::print("file: {}\n", file.file());
    co_await closeFile(loop, file);
  };

  run_loop(setup);
}

TEST(FsTest, FileNotFound) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    try {
      co_await openFile(loop, "/dev/does_not_exist", O_RDONLY);
      EXPECT_FALSE(true);
    } catch (const UvcoException &e) {
      EXPECT_EQ(e.status, UV_ENOENT);
    }
  };

  run_loop(setup);
}

} // namespace
