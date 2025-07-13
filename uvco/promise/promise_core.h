// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "uvco/exception.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/loop/loop.h"

#include <boost/assert.hpp>
#include <fmt/core.h>
#include <fmt/format.h>

#include <coroutine>
#include <cstdio>
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

template <typename T> class PromiseCore;

/// Used as awaiter returned initial_suspend, recording the coroutine handle of
/// the running coroutine.
class CoroutineCaptureAwaiter {
public:
  explicit CoroutineCaptureAwaiter(std::coroutine_handle<> &handle)
      : handle_{handle} {}

  [[nodiscard]] bool await_ready() const {
    // This is always true, because we want to capture the coroutine handle.
    return false;
  }
  [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) const {
    // Set the handle of the running coroutine, so that it can be cancelld
    // later.
    handle_ = handle;
    return false;
  }

  void await_resume() const {}

private:
  std::coroutine_handle<> &handle_;
};

/// A `PromiseCore` is shared among copies of promises waiting for the same
/// coroutine. It contains a state, a result (of type `T`), and potentially a
/// `coroutine_handle` of the coroutine waiting on it. Only one coroutine may
/// await a promise, this is enforced here.
///
/// A PromiseCore is `RefCounted`; this reduces the overhead of `shared_ptr` by
/// as much as 50% in Debug mode and 30% in clang Release mode. However, this is
/// only expected to occur in promise-heavy code without involvement of libuv
/// (such as pure channels).
///
/// The canonical way would be just using `shared_ptr`, which is likely fast
/// enough. But we're experimenting, so let's have fun.
template <typename T> class PromiseCore : public RefCounted<PromiseCore<T>> {
public:
  PromiseCore() = default;

  // The promise core is pinned in memory until the coroutine has finished.
  PromiseCore(const PromiseCore &) = delete;
  PromiseCore(PromiseCore &&) = delete;
  PromiseCore &operator=(const PromiseCore &) = delete;
  PromiseCore &operator=(PromiseCore &&) = delete;
  explicit PromiseCore(T &&value)
      : slot{std::move(value)}, state_{PromiseState::finished} {}

  CoroutineCaptureAwaiter captureCoroutine() {
    return CoroutineCaptureAwaiter{running_};
  }

  /// Set the coroutine to be resumed once a result is ready.
  virtual void setHandle(std::coroutine_handle<> handle) {
    if (state_ != PromiseState::init) {
      throw UvcoException("PromiseCore is already awaited or has finished");
    }
    handle_ = handle;
    state_ = PromiseState::waitedOn;
  }

  /// Reset the handle, so that the coroutine is not resumed anymore. This is
  /// required for SelectSet.
  void resetHandle() {
    BOOST_ASSERT((state_ == PromiseState::waitedOn && handle_) ||
                 (state_ == PromiseState::finished && !handle_) ||
                 (state_ == PromiseState::init && !handle_));
    if (state_ == PromiseState::waitedOn) {
      handle_.reset();
      state_ = PromiseState::init;
    }
  }

  /// Cancel a promise. The awaiter, if present, will immediately receive an
  /// exception. The coroutine itself will keep running, however. (This may be
  /// changed later)
  void cancel() {
    fmt::print(stderr,
               "PromiseCore::cancel() called on promise core of type {}\n",
               typeid(T).name());
    if (state_ == PromiseState::init || state_ == PromiseState::waitedOn) {
      BOOST_ASSERT(!ready());
      fmt::print(stderr,
                 "PromiseCore::cancel() called on promise core in state {}, "
                 "handle = {}\n",
                 static_cast<int>(state_), handle_.has_value());
      if (running_ && !running_.done()) {
        fmt::println("cancelling {}", running_.address());
        Loop::cancel(running_);
        running_.destroy();
        running_ = nullptr;
        if (handle_) {
          handle_->destroy();
          Loop::cancel(*handle_);
          handle_.reset();
          state_ = PromiseState::finished;
        }
      }
      // Fill the slot with an exception, so that the coroutine can be resumed.
      // Double-check `if` for release builds.
      if (!slot) {
        slot = std::make_exception_ptr(
            UvcoException(UV_ECANCELED, "Promise cancelled"));
      }
      resume();
    }
    // else: the underlying coroutine has already returned, so there is no need
    // to cancel it.
  }

  /// Checks if a coroutine is waiting on a promise belonging to this core.
  bool willResume() { return handle_.has_value(); }
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
    if (handle_.has_value()) {
      BOOST_ASSERT(state_ == PromiseState::waitedOn);
      state_ = PromiseState::resuming;
      const std::coroutine_handle<> resume = *handle_;
      handle_.reset();
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
      handle_->destroy();
    }
  }

  void except(const std::exception_ptr &exc) { slot = exc; }

  /// The slot contains the result once obtained.
  std::optional<std::variant<T, std::exception_ptr>> slot;

protected:
  /// Handle of the coroutine to be awakened once the awaited promise has
  /// finished.
  std::optional<std::coroutine_handle<>> handle_;
  /// Handle of the coroutine that is being awaited.
  std::coroutine_handle<> running_;
  PromiseState state_ = PromiseState::init;
};

/// A `void` PromiseCore works like a normal `PromiseCore`, but with the
/// specialization of not transferring values - only control is switched from
/// the yielding to the awaiting coroutine.
template <> class PromiseCore<void> : public RefCounted<PromiseCore<void>> {
public:
  PromiseCore() = default;
  PromiseCore(const PromiseCore &) = delete;
  PromiseCore(PromiseCore &&) = delete;
  PromiseCore<void> &operator=(const PromiseCore &) = delete;
  PromiseCore<void> &operator=(PromiseCore &&) = delete;
  ~PromiseCore() override;

  /// See `PromiseCore::cancel`.
  void cancel();

  CoroutineCaptureAwaiter captureCoroutine() {
    return CoroutineCaptureAwaiter{running_};
  }

  /// See `PromiseCore::set_resume`.
  void setHandle(std::coroutine_handle<> handle);

  /// See `PromiseCore::resetHandle`.
  void resetHandle();
  /// See `PromiseCore::willResume`.
  [[nodiscard]] bool willResume() const;
  /// See `PromiseCore::ready`.
  [[nodiscard]] bool ready() const;
  /// See `PromiseCore::stale`.
  [[nodiscard]] bool stale() const;

  /// See `PromiseCore::resume`.
  void resume();

  /// See `PromiseCore::except`.
  void except(std::exception_ptr exc);

  /// In a void promise, we only track *if* the coroutine has finished, because
  /// it doesn't return anything.
  bool ready_ = false;

  /// Contains the exception if thrown.
  std::optional<std::exception_ptr> exception_;

private:
  std::optional<std::coroutine_handle<>> handle_;
  std::coroutine_handle<> running_;
  PromiseState state_ = PromiseState::init;
};

/// @}

} // namespace uvco
