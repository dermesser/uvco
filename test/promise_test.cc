
#include "loop/loop.h"
#include "promise/promise.h"
#include "test_util.h"
#include "timer.h"

#include <gtest/gtest.h>
#include <utility>

namespace {
using namespace uvco;

TEST(PromiseTest, moveCtor) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    Promise<int> promise1 = []() -> uvco::Promise<int> { co_return 1; }();
    Promise<int> promise2 = std::move(promise1);
    EXPECT_EQ(co_await promise2, 1);
  };

  run_loop(setup);
}

TEST(PromiseTest, destroyWithoutResume) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    Promise<int> promise = [&loop]() -> uvco::Promise<int> {
      co_await sleep(loop, 1);
      // Will put promise core in state finished, all good.
      co_return 1;
    }();
    co_return;
  };

  run_loop(setup);
}

} // namespace
