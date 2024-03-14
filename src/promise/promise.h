// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "exception.h"
#include "promise_core.h"

#include <boost/assert.hpp>
#include <fmt/format.h>

#include <coroutine>
#include <exception>

namespace uvco {

/// @addtogroup Promise
/// @{

/// A `Promise` is the core type of `uvco`, and returned from coroutines. A
/// coroutine is a function containing either of `co_await`, `co_yield`, or
/// `co_return`.
///
/// A Promise can be awaited on; for this the inner type `PromiseAwaiter_` is
/// used. The `PromiseCore` manages the low-level resumption while the `Promise`
/// and `PromiseAwaiter_` types fulfill the C++ standard coroutine protocol.
///
/// A Promise that is being awaited (`co_await promise;`) registers the state of
/// the awaiting coroutine. Once it is fulfilled from elsewhere - almost always
/// another coroutine reaching `co_return` -- the awaiting coroutine is resumed
/// immediately on the stack of the returning coroutine.
///
/// This may lead to a relatively deep stack. The root of the stack
/// is either a libuv callback, or `LoopData::runAll` (the scheduler's run
/// method). As rule of thumb, once a libuv suspension point is reached, e.g. a
/// socket read, the entire stack will be collapsed and control is transferred
/// back to libuv.
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

  /// Part of the coroutine protocol: Called by `co_return`.
  ///
  /// In `uvco`, this method also resumes and runs the awaiting coroutine until
  /// its next suspension point or `co_return`. This may lead to deep stacks if
  /// a long chain of coroutines is resolved at once.
  ///
  /// The upside is that coroutines are run immediately after an event
  /// occurring, reducing latency.
  void return_value(T &&value) {
    BOOST_ASSERT(!core_->slot);
    core_->slot = std::move(value);
    // TODO: don't resume immediately, but schedule resumption. The promise is
    // only destroyed after resume() returns, this has the promise hang around
    // longer than needed.
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
                       "promise is already being waited on!\n");
      core_->set_resume(handle);
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

  /// Construct a Promise<void> that is immediately ready. Usually not
  /// necessary: just use a coroutine which immediately `co_return`s.
  static Promise<void> immediate();

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
  /// Part of the coroutine protocol: raises the exception to the caller or
  /// resumer
  void unhandled_exception();

  /// Returns an awaiter object for the promise, handling actual suspension and
  /// resumption.
  PromiseAwaiter_ operator co_await() { return PromiseAwaiter_{core_}; }

  /// Returns whether the promise has already been fulfilled.
  bool ready() { return core_->ready; }

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
    bool await_ready() const;
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
