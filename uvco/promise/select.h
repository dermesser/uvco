// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <fmt/core.h>
#include <fmt/ranges.h>

#include "uvco/promise/promise.h"

#include <coroutine>
#include <functional>
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
/// std::vector<std::variant<Promise<int>, Promise<int>>> results = co_await
/// SelectSet{promise1, promise2};
/// ```
///
/// It is okay to add an already finished promise to a SelectSet.
///
/// It is possible that no events are returned ("spurious wakeup"); make sure
/// that you can handle an empty result vector.
template <typename... Ts> class SelectSet {
public:
  using Variant = std::variant<std::reference_wrapper<Promise<Ts>>...>;
  using Tuple = std::tuple<std::reference_wrapper<Promise<Ts>>...>;

  explicit SelectSet(Promise<Ts> &...promises) : promises_{promises...} {}

  ~SelectSet() {
    if (!resumed_) {
      std::apply(
          [](auto &&...promise) { (promise.get().core()->resetHandle(), ...); },
          promises_);
    }
  }

  [[nodiscard]] bool await_ready() const noexcept {
    return resumed_ || std::apply(
                           [](auto &&...promise) -> bool {
                             return (promise.get().ready() || ...);
                           },
                           promises_);
  }

  /// Register the current coroutine to be resumed when one of the promises is
  /// ready.
  void await_suspend(std::coroutine_handle<> handle) {
    BOOST_ASSERT_MSG(!resumed_, "A select set can only be used once");
    std::apply(
        [handle](auto &&...promise) {
          ((!promise.get().core()->stale()
                ? promise.get().core()->setHandle(handle)
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
    return readyPromises;
  }

private:
  template <size_t Ix = 0>
  void checkPromises(std::vector<Variant> &readyPromises) {
    if constexpr (Ix < sizeof...(Ts)) {
      using PromiseRefType = typename std::tuple_element_t<Ix, Tuple>;
      PromiseRefType promise = std::get<Ix>(promises_);
      if (promise.get().ready()) {
        readyPromises.emplace_back(std::in_place_index<Ix>, promise);
      } else {
        promise.get().core()->resetHandle();
      }
      checkPromises<Ix + 1>(readyPromises);
    }
  }

  Tuple promises_;
  bool resumed_ = false;
};

/// @}

} // namespace uvco
