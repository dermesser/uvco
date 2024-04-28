// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "exception.h"
#include "internal/internal_utils.h"
#include "promise_core.h"

#include <boost/assert.hpp>
#include <fmt/format.h>

#include <coroutine>
#include <exception>
#include <utility>

namespace uvco {

/// @addtogroup Promise
/// @{

/// A `Promise` is the core type of `uvco`, and returned from coroutines. A
/// coroutine is a function containing either of `co_await`, `co_yield`, or
/// `co_return`. The `Promise` type is therefore the *promise object* of a
/// coroutine, in terms of the C++ standard.
///
/// A Promise doesn't need to be constructed directly; it is returned from a
/// coroutine function. Declare a function with a return type of `Promise<T>`
/// and use `co_return` to return a value - that's it! Inside the coroutine, you
/// can use `co_await` etc.
///
/// When a Promise is awaited using `co_await`, the awaiting coroutine is
/// suspended until the promise is resolved. Once the promise is resolved, the
/// suspended coroutine is scheduled to be resumed by `Loop` at a later time.
///
/// The internal state is held in a `PromiseCore_` shared by all copies of the
/// same `Promise`. However, only one coroutine can await a promise at a time.
template <typename T> class Promise {
protected:
  struct PromiseAwaiter_;
  /// PromiseCore_ handles the inner mechanics of resumption and suspension.
  using PromiseCore_ = PromiseCore<T>;
  using SharedCore_ = PromiseCore_ *;

public:
  /// Part of the coroutine protocol: specifies which class will define the
  /// coroutine behavior. In this case, the `Promise<T>` class is both the
  /// return type and the promise type.
  ///
  /// Note that the awaiter type is separate (`PromiseAwaiter_`).
  using promise_type = Promise<T>;

  /// Unfulfilled, empty promise.
  Promise() : core_{makeRefCounted<PromiseCore_>()} {}
  /// Fulfilled promise; resolves immediately.
  explicit Promise(T &&result)
      : core_{makeRefCounted<PromiseCore_>(std::move(result))} {}

  Promise(Promise<T> &&other) noexcept : core_{other.core_} {
    other.core_ = nullptr;
  }
  /// A promise can be copied at low cost.
  Promise &operator=(const Promise<T> &other) {
    if (this == &other) {
      return *this;
    }
    core_ = other.core_->addRef();
    return *this;
  }
  Promise &operator=(Promise<T> &&other) noexcept {
    if (this == &other) {
      return *this;
    }
    if (core_ != nullptr) {
      core_->delRef();
    }
    core_ = other.core_;
    other.core_ = nullptr;
    return *this;
  }
  // A promise can be copied at low cost.
  Promise(const Promise<T> &other) : core_{other.core_->addRef()} {}
  ~Promise() {
    if (core_ != nullptr) {
      core_->delRef();
    }
  }

  /// Part of the coroutine protocol: Called on first suspension point
  /// (`co_await`) or `co_return`.
  Promise<T> get_return_object() { return *this; }

  /// Part of the coroutine protocol: Called by `co_return`. Schedules the
  /// awaiting coroutine for resumption.
  void return_value(T &&value) {
    BOOST_ASSERT(!core_->slot);
    core_->slot = std::move(value);
    core_->resume();
  }

  /// Part of the coroutine protocol: called after construction of Promise
  /// object, i.e. before starting the coroutine.
  ///
  /// In `uvco`, the coroutine always runs at least up to its first suspension
  /// point, at which point it may be suspended (if the awaited object is not
  /// ready).
  ///
  // Note: if suspend_always is chosen, we can better control when the promise
  // will be scheduled.
  std::suspend_never initial_suspend() noexcept { return {}; }
  /// Part of the coroutine protocol: called upon `co_return` or unhandled
  /// exception.
  std::suspend_never final_suspend() noexcept { return {}; }

  // Part of the coroutine protocol: called upon unhandled exception leaving the
  // coroutine.
  void unhandled_exception() {
    core_->except(std::current_exception());
    core_->resume();
  }

  /// Part of the coroutine protocol: called by `co_await p` where `p` is a
  /// `Promise<T>`. The returned object is awaited on.
  PromiseAwaiter_ operator co_await() { return PromiseAwaiter_{core_}; }

  /// Returns if promise has been fulfilled.
  bool ready() { return core_->slot_.has_value(); }

  T unwrap() {
    if (ready()) {
      auto &slot = core_->slot.value();
      switch (slot.index()) {
      case 0: {
        return std::move(std::get<0>(slot));
      case 1:
        std::rethrow_exception(std::get<1>(slot));
      default:
        throw UvcoException("PromiseAwaiter_::await_resume: invalid slot");
      }
      }
    } else {
      throw UvcoException("unwrap called on unfulfilled promise");
    }
  }

protected:
  /// Returned as awaiter object when `co_await`ing a promise.
  ///
  /// Handles suspension of current coroutine and resumption upon fulfillment of
  /// the awaited promise.
  struct PromiseAwaiter_ {
    /// The `core` is shared with the promise and contains the resumption
    /// handle, and ultimately the returned value.
    explicit PromiseAwaiter_(SharedCore_ core)
        : core_{std::move(core->addRef())} {}
    PromiseAwaiter_(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_(const PromiseAwaiter_ &) = delete;
    PromiseAwaiter_ &operator=(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_ &operator=(const PromiseAwaiter_ &) = delete;
    ~PromiseAwaiter_() { core_->delRef(); }

    /// Part of the coroutine protocol: returns `true` if the promise is already
    /// fulfilled.
    [[nodiscard]] bool await_ready() const { return core_->slot.has_value(); }
    /// Part of the coroutine protocol: returns if suspension is desired (always
    /// true), and stores the awaiting coroutine state in the `PromiseCore`.
    bool await_suspend(std::coroutine_handle<> handle) {
      BOOST_ASSERT_MSG(!core_->willResume(),
                       "promise is already being waited on!");
      core_->setHandle(handle);
      return true;
    }
    /// Part of the coroutine protocol: extracts the resulting value from the
    /// promise core and returns it.
    T await_resume() {
      if (core_->slot.has_value()) {
        switch (core_->slot->index()) {
        case 0: {
          T result = std::move(std::get<0>(core_->slot.value()));
          core_->slot.reset();
          return std::move(result);
        }
        case 1:
          std::rethrow_exception(std::get<1>(core_->slot.value()));
        default:
          throw UvcoException("PromiseAwaiter_::await_resume: invalid slot");
        }
      } else {
        throw UvcoException(
            "await_resume called on unfulfilled promise (bug?)");
      }
    }

    SharedCore_ core_;
  };

  SharedCore_ core_;
};

/// A void promise works slightly differently than a `Promise<T>` in that it
/// doesn't return a value. However, aside from `return_void()` being
/// implemented instead of `return_value()`, the mechanics are identical.
template <> class Promise<void> {
  struct PromiseAwaiter_;
  using SharedCore_ = PromiseCore<void> *;

public:
  /// Part of the coroutine protocol: `Promise<void>` is both return type and
  /// promise type.
  using promise_type = Promise<void>;

  /// Promise ready to be awaited or fulfilled.
  Promise() : core_{makeRefCounted<PromiseCore<void>>()} {}
  Promise(Promise<void> &&other) noexcept;
  Promise &operator=(const Promise<void> &other);
  Promise &operator=(Promise<void> &&other) noexcept;
  Promise(const Promise<void> &other);
  ~Promise();

  /// Part of the coroutine protocol.
  Promise<void> get_return_object() { return *this; }
  /// Part of the coroutine protocol: `uvco` coroutines always run until the
  /// first suspension point.
  std::suspend_never initial_suspend() noexcept { return {}; }
  /// Part of the coroutine protocol: nothing happens upon the final suspension
  /// point (after `co_return`).
  std::suspend_never final_suspend() noexcept { return {}; }

  /// Part of the coroutine protocol: resumes an awaiting coroutine, if there is
  /// one.
  void return_void();
  /// Part of the coroutine protocol: store exception in core and resume
  /// awaiting coroutine.
  void unhandled_exception();

  /// Returns an awaiter object for the promise, handling actual suspension and
  /// resumption.
  PromiseAwaiter_ operator co_await() { return PromiseAwaiter_{core_}; }

  /// Returns whether the promise has already been fulfilled.
  bool ready() { return core_->ready; }

  // Get the result *right now*, and throw an exception if the promise
  // is not ready, or if it encountered an exception itself.
  void unwrap();

private:
  /// Handles the actual suspension and resumption.
  struct PromiseAwaiter_ {
    /// The `core` is shared among all copies of this Promise and holds the
    /// resumption handle to a waiting coroutine, as well as the ready state.
    explicit PromiseAwaiter_(SharedCore_ core) : core_{std::move(core)} {}
    PromiseAwaiter_(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_(const PromiseAwaiter_ &) = delete;
    PromiseAwaiter_ &operator=(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_ &operator=(const PromiseAwaiter_ &) = delete;

    /// Part of the coroutine protocol: returns if the promise is already
    /// fulfilled.
    [[nodiscard]] bool await_ready() const;
    /// Part of the coroutine protocol: returns if suspension is desired (always
    /// true), and stores the awaiting coroutine state in the `PromiseCore`.
    bool await_suspend(std::coroutine_handle<> handle);
    void await_resume();

    SharedCore_ core_;
  };
  SharedCore_ core_;
};

/// @}

} // namespace uvco
