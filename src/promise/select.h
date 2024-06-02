// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "promise/promise.h"
#include <coroutine>
#include <fmt/core.h>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace uvco {

/// @addtogroup Promise
///
/// @{

/// A `SelectSet` is a set of promises that are awaited simultaneously. The
/// first promise that is ready is returned. If no promise is ready, the
/// coroutine is suspended until one of the promises is ready.
///
/// The SelectSet is directly awaitable. For example:
///
/// ```cpp
/// Promise<int> promise1 = []() -> Promise<int> { co_return 1; }();
/// Promise<int> promise2 = []() -> Promise<int> { co_return 2; }();
/// std::vector<std::variant<Promise<int>, Promise<int>>> results = co_await SelectSet{promise1, promise2};
/// ```
template <typename... Ts> class SelectSet {
public:
  using Variant = std::variant<Promise<Ts>...>;
  using Tuple = std::tuple<Promise<Ts>...>;

  explicit SelectSet(Promise<Ts>... promises)
      : promises_{std::move(promises)...} {}

  [[nodiscard]] bool await_ready() const noexcept {
    return resumed_ ||
           std::apply(
               [](auto &&...promise) { return (promise.ready() || ...); },
               promises_);
  }

  void await_suspend(std::coroutine_handle<> handle) {
    std::apply(
        [handle](auto &&...promise) {
          ((!promise.core()->finished() ? promise.core()->setHandle(handle)
                                        : (void)0),
           ...);
        },
        promises_);
  }

  std::vector<Variant> await_resume() {
    BOOST_ASSERT_MSG(!resumed_, "A select set can only be used once");
    resumed_ = true;
    std::vector<Variant> readyPromises;
    checkPromises(readyPromises);
    BOOST_ASSERT_MSG(!readyPromises.empty(),
                     "SelectSet::await_resume: no promise is ready (bug!)");
    return readyPromises;
  }

private:
  template <size_t Ix = 0>
  void checkPromises(std::vector<Variant> &readyPromises) {
    if constexpr (Ix < sizeof...(Ts)) {
      using PromiseType = std::tuple_element_t<Ix, Tuple>;
      PromiseType &promise = std::get<Ix>(promises_);
      if (promise.ready()) {
        readyPromises.emplace_back(std::in_place_index<Ix>, std::move(promise));
      } else {
        promise.core()->resetHandle();
      }
      checkPromises<Ix + 1>(readyPromises);
    }
  }

  Tuple promises_;
  bool resumed_ = false;
};

/// @}

} // namespace uvco
