#include <fmt/core.h>
#include <gtest/gtest.h>

#include "test_util.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/promise.h"
#include "uvco/promise/select.h"
#include "uvco/run.h"

using namespace uvco;

namespace {

TEST(SelectTest, selectBasic) {
  auto setup = [](const Loop &loop) -> uvco::Promise<void> {
    Promise<int> promise1 = []() -> uvco::Promise<int> {
      co_await yield();
      co_return 123;
    }();
    Promise<int> promise2 = []() -> uvco::Promise<int> {
      co_await yield();
      co_await yield();
      co_return 234;
    }();

    auto selectSet = SelectSet{promise1, promise2};
    auto selected = co_await selectSet;
    EXPECT_EQ(selected.size(), 1);
    EXPECT_EQ(co_await std::get<0>(selected[0]), 123);

    // Selecting an already finished promise is okay.
    auto selectSet2 = SelectSet{promise1, promise2};
    auto selected2 = co_await selectSet2;
    EXPECT_EQ(selected2.size(), 1);
    EXPECT_EQ(co_await std::get<1>(selected2[0]), 234);
  };

  run_loop(setup);
}

TEST(SelectTest, selectReturnsSimultaneously) {
  auto simultaneousSelect = [](const Loop &loop) -> Promise<void> {
    auto promise1 = []() -> uvco::Promise<int> {
      co_await yield();
      co_return 1;
    };
    auto promise2 = []() -> uvco::Promise<int> {
      co_await yield();
      co_return 2;
    };

    auto promiseObject1 = promise1();
    auto promiseObject2 = promise2();

    auto selectSet = SelectSet{promiseObject1, promiseObject2};
    auto selected = co_await selectSet;
    EXPECT_EQ(selected.size(), 2);
    EXPECT_EQ(co_await std::get<0>(selected[0]), 1);
    EXPECT_EQ(co_await std::get<1>(selected[1]), 2);
    co_return;
  };

  run_loop(simultaneousSelect);
}

TEST(SelectTest, selectSetMany) {
  auto firstPass = [](const Loop &loop) -> Promise<void> {
    auto promise1 = []() -> uvco::Promise<int> {
      co_await yield();
      co_return 1;
    };
    auto promise2 = []() -> uvco::Promise<int> {
      co_await yield();
      co_await yield();
      co_return 2;
    };
    auto promise3 = []() -> uvco::Promise<int> {
      co_await yield();
      co_await yield();
      co_return 3;
    };
    auto promise4 = []() -> uvco::Promise<int> {
      co_await yield();
      co_await yield();
      co_return 4;
    };

    auto promiseObject1 = promise1();
    auto promiseObject1a = promise1();
    auto promiseObject2 = promise2();
    auto promiseObject2a = promise2();
    auto promiseObject3 = promise3();
    auto promiseObject3a = promise3();
    auto promiseObject4 = promise4();
    auto promiseObject4a = promise4();

    auto selectSet = SelectSet{
        promiseObject1,  promiseObject2,  promiseObject3,  promiseObject4,
        promiseObject1a, promiseObject2a, promiseObject3a, promiseObject4a};
    const auto selected = co_await selectSet;
    EXPECT_EQ(selected.size(), 2);
    EXPECT_EQ(co_await std::get<0>(selected[0]), 1);
    EXPECT_EQ(co_await std::get<4>(selected[1]), 1);
    co_return;
  };

  run_loop(firstPass);
}

} // namespace
