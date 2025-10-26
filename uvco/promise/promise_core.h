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
  /// After the coroutine has been resumed, and is scheduled to be run on the
  /// next Loop turn.
  resuming = 2,
  /// A coroutine has returned the value and the promise is either ready to be
  /// resolved, or has already been resolved (ready() vs stale()).
  finished = 3,
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

  explicit PromiseCore(T &&value)
      : slot{std::move(value)}, state_{PromiseState::finished} {}

  /// Set the coroutine to be resumed once a result is ready.
  virtual void setHandle(std::coroutine_handle<> handle) {
    if (state_ != PromiseState::init) {
      throw UvcoException("PromiseCore is already awaited or has finished");
    }
    handle_ = handle;
    state_ = PromiseState::waitedOn;
  }
  void setRunning(std::coroutine_handle<> handle) {
    BOOST_ASSERT(state_ == PromiseState::init);
    coroutine_ = handle;
  }

  /// Reset the handle, so that the coroutine is not resumed anymore. This is
  /// required for SelectSet.
  void resetHandle() {
    BOOST_ASSERT((state_ == PromiseState::waitedOn && handle_) ||
                 (state_ == PromiseState::finished && !handle_) ||
                 (state_ == PromiseState::init && !handle_));
    if (state_ == PromiseState::waitedOn) {
      handle_ = nullptr;
      state_ = PromiseState::init;
    }
  }

  /// Checks if a coroutine is waiting on a promise belonging to this core.
  bool isAwaited() { return handle_ != nullptr; }

  /// Checks if a value is present in the slot.
  [[nodiscard]] bool ready() const { return slot.has_value(); }
  /// Checks if the coroutine has returned, and the results have been fetched
  /// (i.e. after co_return -> co_await).
  [[nodiscard]] bool stale() const {
    return state_ == PromiseState::finished && !ready();
  }

  /// Resume a suspended coroutine waiting on the associated coroutine by
  /// enqueuing it in the global event loop.
  ///
  /// A promise core can only be resumed once.
  void resume() {
    if (handle_) {
      BOOST_ASSERT(state_ == PromiseState::waitedOn);
      state_ = PromiseState::resuming;
      const std::coroutine_handle<> resume = handle_;
      handle_ = nullptr;
      Loop::enqueue(resume);
    } else {
      // This occurs if no co_await has occured until resume. Either the
      // promise was not co_awaited, or the producing coroutine immediately
      // returned a value. (await_ready() == true)
    }

    switch (state_) {
      // Coroutine returns but nobody has awaited yet. This is fine.
    case PromiseState::init:
      // Not entirely correct, but the resumed awaiting coroutine is not coming
      // back to us.
    case PromiseState::resuming:
      state_ = PromiseState::finished;
      break;
    case PromiseState::waitedOn:
      // state is waitedOn, but no handle is set - that's an error.
      BOOST_ASSERT_MSG(
          false,
          "PromiseCore::resume() called without handle in state waitedOn");
      break;
    case PromiseState::finished:
      // Happens in MultiPromiseCore on co_return if the co_awaiter has lost
      // interest. Harmless if !handle_ (asserted above).
      break;
    }
  }

  void destroyCoroutine() {
    if (coroutine_) {
      Loop::cancel(coroutine_);
      coroutine_.destroy();
    }
  }

  /// Destroys a promise core. Also destroys a coroutine if there is one
  /// suspended and has not been resumed yet. In that case, a warning is
  /// emitted ("PromiseCore destroyed without ever being resumed").
  virtual ~PromiseCore() {
    if (state_ != PromiseState::finished) {
      fmt::print(
          stderr,
          "PromiseCore destroyed without ever being resumed ({}, state = {})\n",
          typeid(T).name(), static_cast<int>(state_));
    }
    // This only happens if the awaiting coroutine has never been resumed, but
    // the last promise provided by it is gone (in turn calling
    // ~PromiseCore()). Important: we may only destroy a suspended coroutine,
    // not a finished one.
    if (handle_) {
      handle_.destroy();
    }
  }

  void except(const std::exception_ptr &exc) { slot = exc; }

  /// The slot contains the result once obtained.
  std::optional<std::variant<T, std::exception_ptr>> slot;

protected:
  // May be nullptr. Set to the coroutine awaiting this promise.
  std::coroutine_handle<> handle_;
  // Set to the coroutine producing this promise.
  std::coroutine_handle<> coroutine_;
  static_assert(sizeof(std::coroutine_handle<>) <= sizeof(void *),
                "coroutine_handle is too large");
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
  std::coroutine_handle<> waitingHandle_;
  std::coroutine_handle<> coroutine_;
  PromiseState state_ = PromiseState::init;
};

/// @}

} // namespace uvco
