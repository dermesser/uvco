// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "uvco/exception.h"
#include "uvco/loop/loop.h"

#include <boost/assert.hpp>
#include <fmt/core.h>
#include <fmt/format.h>

#include <coroutine>
#include <exception>
#include <optional>
#include <typeinfo>
#include <utility>
#include <uv.h>
#include <variant>

namespace uvco {
/// @addtogroup Promise
/// @{

/// Valid states for a promise to be in:
///
/// Initially, `init` marks a newly constructed promise. Once a coroutine waits
/// for its result, the promise transitions to `waitedOn`. At that point, the
/// `handle_` field contains a resume handle (of the waiter). Once the promise
/// is ready and the caller is resumed, the state transitions to `running`.
/// After the caller has been run (and suspended again), the state is
/// `finished`, and no more operations may be executed on this promise.
enum class PromiseState {
  /// After construction, as the associated coroutine is about to start, up to
  /// the first suspension point and the following co_await.
  init = 0,
  /// After the coroutine has reached a suspension point and another coroutine
  /// has started co_awaiting it.
  waitedOn = 1,
  /// A coroutine has returned the value and the promise is either ready to be
  /// resolved, or has already been resolved (ready() vs stale()).
  finished = 2,
};

/// A `PromiseCore` is part of the Coroutine<T> frame; a reference to it is held
/// by the Promise referring to the coroutine.
template <typename T> class PromiseCore {
public:
  PromiseCore() = default;
  PromiseCore(const PromiseCore &) = delete;
  PromiseCore(PromiseCore &&) = delete;
  PromiseCore &operator=(const PromiseCore &) = delete;
  PromiseCore &operator=(PromiseCore &&) = delete;

  virtual ~PromiseCore() {
    if (state_ != PromiseState::finished) {
      // This note means that a promise and its coroutine were destroyed
      // (cancelled) before ever finishing. This is valid in principle, but may
      // be unexpected. For our unit tests, we want to know when this happens so
      // we can react to it.
      fmt::print(stderr,
                 "PromiseCore destroyed without ever being resumed ({}, state "
                 "= {}) (dropped by accident?)\n",
                 typeid(T).name(), static_cast<int>(state_));
    }
  }

  explicit PromiseCore(T &&value)
      : slot{std::move(value)}, state_{PromiseState::finished} {}

  /// Set the coroutine to be resumed once a result is ready.
  virtual void setHandle(std::coroutine_handle<> handle) {
    if (state_ != PromiseState::init) {
      throw UvcoException("PromiseCore is already awaited or has finished");
    }
    waitingHandle_ = handle;
    state_ = PromiseState::waitedOn;
  }

  /// Used by `Coroutine<T>` to set the producing coroutine handle.
  void setRunning(std::coroutine_handle<> handle) {
    BOOST_ASSERT(state_ == PromiseState::init);
    coroutine_ = handle;
  }

  /// Reset the handle, so that the coroutine is not resumed anymore. This is
  /// required for SelectSet.
  void resetHandle() {
    BOOST_ASSERT((state_ == PromiseState::waitedOn && waitingHandle_) ||
                 (state_ == PromiseState::finished && !waitingHandle_) ||
                 (state_ == PromiseState::init && !waitingHandle_));
    if (state_ == PromiseState::waitedOn) {
      waitingHandle_ = nullptr;
      state_ = PromiseState::init;
    }
  }

  /// Checks if a coroutine is waiting on a promise belonging to this core.
  bool isAwaited() { return waitingHandle_ != nullptr; }

  /// Checks if a value is present in the slot.
  [[nodiscard]] bool ready() const { return slot.has_value(); }

  /// Returns true if the promise has completed, and its results have been
  /// fetched.
  [[nodiscard]] bool stale() const {
    return state_ == PromiseState::finished && !ready();
  }

  /// Resume a suspended coroutine waiting on the associated coroutine by
  /// enqueuing it in the global event loop.
  ///
  /// A promise core can only be resumed once.
  void resume() {
    if (waitingHandle_) {
      BOOST_ASSERT(state_ == PromiseState::waitedOn);
      const std::coroutine_handle<> resume = waitingHandle_;
      waitingHandle_ = nullptr;
      Loop::enqueue(resume);
    } else {
      // This occurs if no co_await has occured until resume. Either the
      // promise was not co_awaited, or the producing coroutine immediately
      // returned a value. (await_ready() == true)
    }
    state_ = PromiseState::finished;
  }

  void destroyCoroutine() {
    if (coroutine_) {
      Loop::cancel(coroutine_);
      coroutine_.destroy();
    }
  }

  void except(const std::exception_ptr &exc) { slot = exc; }

  /// The slot contains the result once obtained.
  std::optional<std::variant<T, std::exception_ptr>> slot;

protected:
  /// Set to the coroutine producing this promise. Used to destroy it after
  /// completion or when the associated Promise is dropped.
  std::coroutine_handle<> coroutine_;
  /// Set to the coroutine awaiting this promise if state_ ==
  /// awaitedOn. May be nullptr.
  std::coroutine_handle<> waitingHandle_;
  static_assert(sizeof(std::coroutine_handle<>) <= sizeof(void *),
                "coroutine_handle is unusually large");
  PromiseState state_ = PromiseState::init;
};

/// A `void` PromiseCore works like a normal `PromiseCore`, but with the
/// specialization of not transferring values - only control is switched from
/// the yielding to the awaiting coroutine.
///
/// Contrary to PromiseCore<T>, this is embedded in the coroutine frame itself,
/// saving an allocation.
template <> class PromiseCore<void> {
public:
  PromiseCore() = default;
  PromiseCore(const PromiseCore &) = delete;
  PromiseCore(PromiseCore &&) = delete;
  PromiseCore<void> &operator=(const PromiseCore &) = delete;
  PromiseCore<void> &operator=(PromiseCore &&) = delete;
  ~PromiseCore();

  /// See `PromiseCore::set_resume`.
  void setHandle(std::coroutine_handle<> handle);

  /// See `PromiseCore::resetHandle`.
  void resetHandle();
  /// See `PromiseCore::isAwaited`.
  [[nodiscard]] bool isAwaited() const;
  /// See `PromiseCore::ready`.
  [[nodiscard]] bool ready() const;
  /// See `PromiseCore::stale`.
  [[nodiscard]] bool stale() const;

  /// See `PromiseCore::resume`.
  void resume();

  /// See `PromiseCore::except`.
  void except(std::exception_ptr exc);

  // Managing coroutine state.
  void setRunning(std::coroutine_handle<> handle);

  // Destroy the coroutine state associated with this core. Must be routinely
  // invoked after coroutine completion (due to suspend_always returned from
  // final_suspend).
  void destroyCoroutine() {
    if (coroutine_) {
      Loop::cancel(coroutine_);
      coroutine_.destroy();
    }
  }

  /// In a void promise, we only track *if* the coroutine has finished, because
  /// it doesn't return anything.
  bool ready_ = false;

  /// Contains the exception if thrown.
  std::optional<std::exception_ptr> exception_;

private:
  std::coroutine_handle<> coroutine_;
  std::coroutine_handle<> waitingHandle_;
  PromiseState state_ = PromiseState::init;
};

/// @}

} // namespace uvco
