// uvco (c) 2025 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"
#include "uvco/promise/select.h"

namespace uvco {

/// @addtogroup Combinators
/// Functions and classes useful to combine promises and generators into
/// higher-level items.
/// @{

/// Suspend current coroutine until next event loop iteration. This is
/// especially useful when running expensive computations during which I/O
/// should still happen to avoid starving other tasks; or as replacement for
/// short-duration sleeps.
Promise<void> yield();

/// Generate `count` values from 0 to `count - 1`.
MultiPromise<unsigned> yield(unsigned count);

/// Wait on any of the promises to be ready. Returns a vector of variants of
/// possible values; one for each supplied promise that became ready. Non-ready
/// promises are not cancelled.
///
/// This can be called repeatedly to wait until all promises are ready.
///
/// `waitEither()` is only a wrapper around `SelectSet`.
template <typename... PromiseTypes>
Promise<std::vector<std::variant<PromiseTypes...>>>
waitAny(Promise<PromiseTypes> &...promises) {
  using S = SelectSet<PromiseTypes...>;
  using V = std::variant<PromiseTypes...>;
  S selectSet{promises...};
  auto readyPromises = co_await selectSet;
  std::vector<V> results;
  for (auto &promise : readyPromises) {
    results.emplace_back(std::visit([](auto *p) -> V { return p->unwrap(); },
                                    std::move(promise)));
  }
  co_return results;
}

/// Like `waitAny`, but cancel all promises that were not ready in time.
/// Returns a vector of the results that were ready first; most frequently, only
/// one element will be set.
template <typename... PromiseTypes>
Promise<std::vector<std::variant<PromiseTypes...>>>
race(Promise<PromiseTypes>... promises) {
  co_return (co_await waitAny(promises...));
}

/// Wait on any promise to be ready, ignoring results. When any promise is
/// ready, all others are dropped and cancelled. This is very useful to run
/// background promises concurrently, stopping all of them once one of them
/// finishes or fails.
template <typename... PromiseTypes>
Promise<void> raceIgnore(Promise<PromiseTypes>... promises) {
  using S = SelectSet<PromiseTypes...>;
  S selectSet{promises...};
  co_await selectSet;
}

/// @}

} // namespace uvco
