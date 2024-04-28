// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "internal/internal_utils.h"
#include "loop/loop.h"
#include <exception>
#include <fmt/core.h>

#include <boost/assert.hpp>
#include <fmt/format.h>

#include <coroutine>
#include <cstdio>
#include <optional>
#include <typeinfo>
#include <utility>
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
  init = 0,
  waitedOn = 1,
  running = 2,
  finished = 3,
  exception = 4
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

  /// Set the coroutine to be resumed once a result is ready.
  virtual void setHandle(std::coroutine_handle<> handle) {
    BOOST_ASSERT(state_ == PromiseState::init);
    handle_ = handle;
    state_ = PromiseState::waitedOn;
  }

  /// Checks if a coroutine is waiting on this core.
  bool willResume() { return handle_.has_value(); }

  /// Resume a suspended coroutine by enqueuing it in the global event loop.
  ///
  /// A promise core can only be resumed once.
  virtual void resume() {
    if (handle_) {
      BOOST_ASSERT(state_ == PromiseState::waitedOn ||
                   state_ == PromiseState::exception);
      state_ = PromiseState::running;
      auto resume = *handle_;
      handle_.reset();
      Loop::enqueue(resume);
    } else {
      // This occurs if no co_await has occured until resume. Either the promise
      // was not co_awaited, or the producing coroutine immediately returned a
      // value. (await_ready() == true)
    }

    // Note: with asynchronous resumption (Loop::enqueue), this state machine is
    // a bit faulty. The promise awaiter is resumed in state `finished`.
    // However, this works out fine for the purpose of enforcing the "protocol"
    // of interactions with the promise core: a promise can be destroyed without
    // the resumption having run, but that is an issue in the loop or the result
    // of a premature termination.
    switch (state_) {
    case PromiseState::init:
    case PromiseState::running:
      state_ = PromiseState::finished;
      break;
    case PromiseState::waitedOn:
      // It is possible that set_resume() was called in a stack originating at
      // resume(), thus updating the state. In that case, the state should be
      // preserved.
      state_ = PromiseState::waitedOn;
      break;
    case PromiseState::finished:
    case PromiseState::exception:
      // Happens in MultiPromiseCore on co_return if the co_awaiter has lost
      // interest. Harmless if !resume_ (asserted above).
      break;
    }
  }

  /// Destroys a promise core. Also destroys a coroutine if there is one
  /// suspended and has not been resumed yet. In that case, a warning is
  /// emitted ("PromiseCore destroyed without ever being resumed").
  virtual ~PromiseCore() {
    if (state_ != PromiseState::finished && state_ != PromiseState::exception) {
      fmt::print(stderr,
                 "PromiseCore destroyed without ever being resumed ({})\n",
                 typeid(T).name());
    }
    // This only happens if the awaiting coroutine has never been resumed, but
    // the last promise provided by it is gone.
    // Important: we may only destroy a suspended coroutine, not a finished one:
    // co_return already destroys coroutine state.
    if (handle_) {
      handle_->destroy();
    }
  }

  virtual void except(const std::exception_ptr &exc) {
    slot = exc;
    state_ = PromiseState::exception;
  }

  /// The slot contains the result once obtained.
  std::optional<std::variant<T, std::exception_ptr>> slot;

protected:
  std::optional<std::coroutine_handle<>> handle_;
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

  /// See `PromiseCore::set_resume`.
  void setHandle(std::coroutine_handle<> handle);
  /// See `PromiseCore::will_resume`.
  bool willResume();
  /// See `PromiseCore::resume`.
  void resume();

  void except(std::exception_ptr exc);

  bool ready = false;

  std::optional<std::exception_ptr> exception_;

private:
  std::optional<std::coroutine_handle<>> resume_;
  PromiseState state_ = PromiseState::init;
};

/// @}

} // namespace uvco
