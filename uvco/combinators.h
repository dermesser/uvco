// uvco (c) 2025 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"
#include "uvco/promise/select.h"
#include <coroutine>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <tuple>

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
/// `waitAny()` is only a wrapper around `SelectSet`.
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

/// Like `waitAny`, but cancels all promises that were not ready in time.
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

namespace detail {

template <typename T> struct ReplaceVoid {
  using type = T;
};

struct Void {};

template <> struct ReplaceVoid<void> {
  using type = Void;
};

template <typename PromiseType>
Promise<typename ReplaceVoid<PromiseType>::type>
awaitAndReplaceVoid(Promise<PromiseType> &promise) {
  co_return (co_await promise);
}

template <> inline Promise<Void> awaitAndReplaceVoid(Promise<void> &promise) {
  co_await promise;
  co_return {};
}

} // namespace detail

/// Wait for all promises to finish, returning all results in a tuple. In the
/// returned tuple, any `Promise<void>` is represented by a `Void` struct, which
/// is just an empty struct.
template <typename... PromiseTypes>
Promise<std::tuple<typename detail::ReplaceVoid<PromiseTypes>::type...>>
waitAll(Promise<PromiseTypes>... promises) {
  co_return std::make_tuple(co_await detail::awaitAndReplaceVoid(promises)...);
}

/// Allows an arbitrary number of coroutines to wait on another coroutine to
/// fulfill it.
class WaitPoint {
  struct WaitPointAwaiter_;

public:
  WaitPoint() = default;
  WaitPoint(const WaitPoint &) = delete;
  WaitPoint(WaitPoint &&) noexcept = default;
  WaitPoint &operator=(const WaitPoint &) = delete;
  WaitPoint &operator=(WaitPoint &&) noexcept = default;
  ~WaitPoint() = default;

  [[nodiscard]] size_t waiting() const { return waiters_.size(); }
  Promise<void> wait();
  void releaseOne();
  void releaseAll();

private:
  void enqueue(std::coroutine_handle<> handle);
  std::deque<std::coroutine_handle<>> waiters_;
};

/// TaskSet handles the common case of maintaining a set of coroutines running
/// in the background which should not be cancelled, but should be cleaned up
/// after finishing.
class TaskSet {
public:
  TaskSet() = default;
  TaskSet(const TaskSet &) = delete;
  TaskSet(TaskSet &&) noexcept = default;
  TaskSet &operator=(const TaskSet &) = delete;
  TaskSet &operator=(TaskSet &&) noexcept = default;
  virtual ~TaskSet() = default;

  using Id = size_t;
  using ErrorCallback = std::function<void(Id, std::exception_ptr)>;

  /// Create a new TaskSet, which can be used to hold a set of background tasks,
  /// cleaning up resources of finished tasks automatically.
  ///
  /// The implementation is hidden in the .cc file to hide complexity from units
  /// including this header.
  static std::unique_ptr<TaskSet> create();

  /// Add a task to the TaskSet. It will be run to completion. If an exception
  /// is thrown, it is printed to stderr.
  virtual Id add(Promise<void> task) = 0;

  /// Check if there are any active tasks on the TaskSet.
  virtual bool empty() = 0;

  /// Return a Promise which will be fulfilled when no tasks are left on the
  /// TaskSet. Multiple calls to this will return Promise instances which will
  /// become ready simultaneously; this means that depending on the scheduling
  /// order, by the time this Promise resolves, the TaskSet instance may not be
  /// empty anymore.
  virtual Promise<void> onEmpty() = 0;

  /// Register a callback which handles errors thrown by tasks. By default,
  /// errors are logged to stderr. The callback is called when a task finishes
  /// by throwing an exception.
  virtual void setOnError(ErrorCallback callback) = 0;
};

/// @}

} // namespace uvco
