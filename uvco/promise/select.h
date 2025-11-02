// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <fmt/core.h>
#include <fmt/ranges.h>

#include "uvco/promise/promise.h"

#include <coroutine>
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
/// See also `waitAny()` for a more convenient approach that internally utilizes
/// this class.
///
/// The SelectSet is directly awaitable. For example:
///
/// ```cpp
/// Promise<int> promise1 = []() -> Promise<int> { co_return 1; }();
/// Promise<int> promise2 = []() -> Promise<int> { co_return 2; }();
/// std::vector<std::variant<Promise<int>, Promise<int>>> results = co_await
/// SelectSet{promise1, promise2};
/// ASSERT_EQ(2, results.size());
/// EXPECT_EQ(0, results[0].index()); // Order is unspecified usually
/// EXPECT_EQ(1, std::get<0>(results[0]).unwrap()); // or co_await
///
/// // Easy approach, no second co_await:
/// std::vector<std::variant<int, int>> results2 = co_await waitAny(promise1,
/// promise2); ASSERT_EQ(2, results.size()); EXPECT_EQ(0, results[0].index());
/// // Order is unspecified usually
/// ```
///
/// It is okay to add an already finished promise to a SelectSet.
///
/// It is possible that no events are returned ("spurious wakeup"); make sure
/// that you can handle an empty result vector.
template <typename... Ts> class SelectSet {
public:
  using Variant = std::variant<Promise<Ts> *...>;
  using Tuple = std::tuple<Promise<Ts> *...>;

  explicit SelectSet(Promise<Ts> &...promises) : promises_{&promises...} {}

  ~SelectSet() {
    if (!resumed_) {
      std::apply(
          [](auto *...promise) { (promise->core()->resetHandle(), ...); },
          promises_);
    }
  }

  [[nodiscard]] bool await_ready() const noexcept {
    return resumed_ || std::apply(
                           [](auto *...promise) -> bool {
                             return (promise->ready() || ...);
                           },
                           promises_);
  }

  /// Register the current coroutine to be resumed when one of the promises is
  /// ready.
  void await_suspend(std::coroutine_handle<> handle) {
    BOOST_ASSERT_MSG(!resumed_, "A select set can only be used once");
    std::apply(
        [handle](auto *...promise) {
          ((!promise->core()->stale() ? promise->core()->setHandle(handle)
                                      : (void)0),
           ...);
        },
        promises_);
  }

  /// Returns all promises that are ready.
  /// It is possible that no promise is ready, and the returned vector is empty,
  /// in the case that two promises were ready at once; one promise scheduled
  /// the SelectSet for resumption, we delivered both events, and will be woken
  /// up a second time.
  ///
  /// TODO: provide a no-/rare-allocation API by reusing a vector or span.
  std::vector<Variant> await_resume() {
    resumed_ = true;
    std::vector<Variant> readyPromises;
    checkPromises(readyPromises);
    // Prevent the current coroutine from being resumed again, if another
    // promise in the SelectSet became ready in the meantime.
    Loop::cancel(suspended_);
    suspended_ = nullptr;
    return readyPromises;
  }

private:
  template <size_t Ix = 0>
  void checkPromises(std::vector<Variant> &readyPromises) {
    if constexpr (Ix < sizeof...(Ts)) {
      using PromisePtr = std::tuple_element_t<Ix, Tuple>;
      PromisePtr promise = std::get<Ix>(promises_);
      if (promise->ready()) {
        readyPromises.emplace_back(std::in_place_index<Ix>, promise);
      } else {
        // Reset promises to `init` state and remove the current coroutine's
        // handle from them so they don't resume us later.
        promise->core()->resetHandle();
      }
      checkPromises<Ix + 1>(readyPromises);
    }
  }

  Tuple promises_;
  std::coroutine_handle<> suspended_;
  bool resumed_ = false;
};

/// @}

} // namespace uvco
