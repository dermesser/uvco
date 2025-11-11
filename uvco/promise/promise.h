// uvco (c) 2023-2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "uvco/exception.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/promise/promise_core.h"

#include <boost/assert.hpp>
#include <fmt/format.h>

#include <coroutine>
#include <exception>
#include <utility>

namespace uvco {

/// @addtogroup Promise
/// @{

template <typename T> class Coroutine;
template <typename T> class Promise;

/// A `Promise` is the core type of `uvco`, and returned from coroutines. A
/// coroutine is a function containing either of `co_await`, `co_yield`, or
/// `co_return`. The `Promise` type defines `Coroutine` to be the promise type
/// used within a coroutine. The Promise object itself acts as awaitable for
/// awaiting coroutines.
///
/// A Promise doesn't need to be constructed directly; it is always returned
/// from a coroutine function. Declare a function with a return type of
/// `Promise<T>` and use `co_return` to return a value - that's it! Inside the
/// coroutine, you can use `co_await` etc.
///
/// When a Promise is awaited using `co_await`, the awaiting coroutine is
/// suspended until the promise is resolved. Once the promise is resolved, the
/// suspended coroutine is scheduled to be resumed by `Loop` at a later time.
///
/// The internal state is held in a `PromiseCore_` shared by all copies of the
/// same `Promise`. However, only one coroutine can await a (shared) promise at
/// a time.
///
/// See `README.md` for some examples of how to use coroutines and promises.
///
/// NOTE: due to inlining (visibility of the promise code) and other factors,
/// Promise<int> (for example) can be 20% faster than Promise<void>, and also
/// cause fewer allocations. This happens especially when the compiler can elide
/// the coroutine frame allocation for Promise<T>, which it can't do with
/// Promise<void> (which is implemented in a .cc file).
template <typename T> class Promise {
protected:
  struct PromiseAwaiter_;
  /// PromiseCore_ handles the inner mechanics of resumption and suspension.
  using PromiseCore_ = PromiseCore<T>;

public:
  /// Part of the coroutine protocol: specifies which class will define the
  /// coroutine behavior. In this case, the Coroutine class implements the
  /// promise protocol (don't get confused!). This split is useful so that
  /// coroutines can use Promise objects as function arguments without implicit
  /// construction of their promise objects, which can easily cuase bugs.
  ///
  /// Note that the awaiter type is separate (`PromiseAwaiter_`).
  using promise_type = Coroutine<T>;

  /// Unfulfilled, empty promise.
  Promise() : core_{makeRefCounted<PromiseCore_>()} {}
  /// Fulfilled promise; resolves immediately.
  explicit Promise(T &&result)
      : core_{makeRefCounted<PromiseCore_>(std::move(result))} {}

  Promise(Promise<T> &&other) noexcept : core_{other.core_} {
    other.core_ = nullptr;
  }
  /// A promise can be copied at low cost.
  Promise &operator=(const Promise<T> &other) = delete;
  Promise &operator=(Promise<T> &&other) noexcept {
    if (this == &other) {
      return *this;
    }
    if (core_ != nullptr) {
      core_->destroyCoroutine();
    }
    core_ = other.core_;
    other.core_ = nullptr;
    return *this;
  }
  // A promise can be copied at low cost.
  Promise(const Promise<T> &other) = delete;
  ~Promise() {
    if (core_ != nullptr) {
      core_->destroyCoroutine();
    }
  }

  /// Part of the coroutine protocol: called by `co_await p` where `p` is a
  /// `Promise<T>`. The returned object is awaited on.
  PromiseAwaiter_ operator co_await() const { return PromiseAwaiter_{*core_}; }

  /// Returns if promise has been fulfilled.
  [[nodiscard]] bool ready() const { return core_->ready(); }

  T unwrap() {
    if (ready()) {
      auto &slot = core_->slot.value();
      switch (slot.index()) {
      case 0: {
        T value = std::move(std::get<0>(slot));
        core_->slot.reset();
        return value;
      }
      case 1: {
        std::rethrow_exception(std::get<1>(slot));
      }
      default:
        throw UvcoException("PromiseAwaiter_::await_resume: invalid slot");
      }
    }
    throw UvcoException("unwrap called on unfulfilled promise");
  }

protected:
  /// Returned as awaiter object when `co_await`ing a promise.
  ///
  /// Handles suspension of current coroutine and resumption upon fulfillment of
  /// the awaited promise.
  struct PromiseAwaiter_ {
    /// The `core` is shared with the promise and contains the resumption
    /// handle, and ultimately the returned value. Because the awaiter object
    /// is only used while a coroutine is waiting on a co_await suspension
    /// point, we can use a reference to the `PromiseCore_` object.
    explicit PromiseAwaiter_(PromiseCore_ &core) : core_{core} {}
    PromiseAwaiter_(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_(const PromiseAwaiter_ &) = delete;
    PromiseAwaiter_ &operator=(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_ &operator=(const PromiseAwaiter_ &) = delete;
    ~PromiseAwaiter_() = default;

    /// Part of the coroutine protocol: returns `true` if the promise is already
    /// fulfilled.
    [[nodiscard]] bool await_ready() const {
      return core_.ready() && !core_.stale();
    }

    /// Part of the coroutine protocol: returns if suspension is desired (always
    /// true), and stores the awaiting coroutine state in the `PromiseCore`.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) const {
      BOOST_ASSERT_MSG(!core_.isAwaited(),
                       "promise is already being waited on!");
      core_.setHandle(handle);
      return true;
    }
    /// Part of the coroutine protocol: extracts the resulting value from the
    /// promise core and returns it.
    T await_resume() const {
      if (core_.stale()) {
        throw UvcoException(
            "co_await called on previously finished promise (T)");
      }
      if (core_.slot.has_value()) {
        switch (core_.slot->index()) {
        case 0: {
          T result = std::move(std::get<0>(core_.slot.value()));
          core_.slot.reset();
          return result;
        }
        case 1: {
          const auto exc = std::get<1>(core_.slot.value());
          core_.slot.reset();
          std::rethrow_exception(exc);
        }
        default:
          throw UvcoException("PromiseAwaiter_::await_resume: invalid slot");
        }
      }
      throw UvcoException("await_resume called on unfulfilled promise (bug?)");
    }

    PromiseCore_ &core_;
  };

  template <typename U> friend class Coroutine;
  template <typename... Ts> friend class SelectSet;

  // Constructor used by Coroutine<T>
  explicit Promise(PromiseCore_ &core) : core_{&core} {}

  PromiseCore_ *core() { return core_; }

  PromiseCore_ *core_;
};

/// A void promise works slightly differently than a `Promise<T>` in that it
/// doesn't return a value. However, aside from `return_void()` being
/// implemented instead of `return_value()`, the mechanics are identical.
///
/// NOTE: for a transition period, Promise<void> is more efficient than
/// Promise<T>: it does not allocate a separate PromiseCore. Instead, the
/// PromiseCore<void> is placed in the Coroutine<void> frame, which itself is
/// allocated anyway.
template <> class Promise<void> {
  struct PromiseAwaiter_;
  using PromiseCore_ = PromiseCore<void>;

public:
  /// Part of the coroutine protocol: `Promise<void>` is both return type and
  /// promise type.
  using promise_type = Coroutine<void>;

  /// Promise ready to be awaited or fulfilled.
  Promise(Promise<void> &&other) noexcept;
  Promise &operator=(const Promise<void> &other) = delete;
  Promise &operator=(Promise<void> &&other) noexcept;
  Promise(const Promise<void> &other) = delete;
  ~Promise();

  /// Returns an awaiter object for the promise, handling actual suspension and
  /// resumption.
  PromiseAwaiter_ operator co_await() const;

  /// Returns whether the promise has already been fulfilled.
  [[nodiscard]] bool ready() const;

  // Get the result *right now*, and throw an exception if the promise
  // is not ready, or if it encountered an exception itself.
  void unwrap();

private:
  /// Handles the actual suspension and resumption.
  struct PromiseAwaiter_ {
    /// The `core` is shared among all copies of this Promise and holds the
    /// resumption handle to a waiting coroutine, as well as the ready state.
    explicit PromiseAwaiter_(PromiseCore<void> &core);
    PromiseAwaiter_(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_(const PromiseAwaiter_ &) = delete;
    PromiseAwaiter_ &operator=(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_ &operator=(const PromiseAwaiter_ &) = delete;
    ~PromiseAwaiter_() = default;

    /// Part of the coroutine protocol: returns if the promise is already
    /// fulfilled.
    [[nodiscard]] bool await_ready() const;
    /// Part of the coroutine protocol: returns if suspension is desired (always
    /// true), and stores the awaiting coroutine state in the `PromiseCore`.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) const;
    void await_resume() const;

    PromiseCore<void> &core_;
  };

  explicit Promise(PromiseCore_ &core);

  friend class Coroutine<void>;
  template <typename... Ts> friend class SelectSet;

  PromiseCore<void> *core() {
    BOOST_ASSERT(core_ != nullptr);
    return core_;
  }

  PromiseCore<void> *core_;
};

/// A coroutine object used internally by C++20 coroutines ("promise object").
/// This is the coroutine's "promise_type", i.e. one instance will be allocated
/// for every coroutine invocation, and be part of the coroutine frame. The
/// coroutine frame will be stack-allocated if we're lucky.
template <typename T> class Coroutine {
  /// PromiseCore_ handles the inner mechanics of resumption and suspension.
  using PromiseCore_ = PromiseCore<T>;
  using SharedCore_ = PromiseCore_ *;

public:
  /// Coroutine object lives and is pinned within the coroutine frame;
  /// copy/move is disallowed.
  Coroutine() = default;
  Coroutine(const Coroutine &other) = delete;
  Coroutine &operator=(const Coroutine &other) = delete;
  Coroutine(Coroutine &&other) = delete;
  Coroutine &operator=(Coroutine &&other) = delete;
  ~Coroutine() = default;

  /// Part of the coroutine protocol: Called on first suspension point
  /// (`co_await`) or `co_return`.
  Promise<T> get_return_object() { return Promise<T>{core_}; }

  /// Part of the coroutine protocol: Called by `co_return`. Schedules the
  /// awaiting coroutine for resumption.
  void return_value(T value) {
    // Probably cancelled.
    if (core_.slot.has_value() && core_.slot->index() == 1) {
      return;
    }
    BOOST_ASSERT(!core_.slot);
    core_.slot = std::move(value);
    core_.resume();
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
  std::suspend_never initial_suspend() noexcept {
    core_.setRunning(std::coroutine_handle<Coroutine<T>>::from_promise(*this));
    return {};
  }
  /// Part of the coroutine protocol: called upon `co_return` or unhandled
  /// exception.
  std::suspend_always final_suspend() noexcept { return {}; }

  // Part of the coroutine protocol: called upon unhandled exception leaving
  // the coroutine.
  void unhandled_exception() {
    core_.except(std::current_exception());
    core_.resume();
  }

protected:
  PromiseCore<T> core_;
};

template <> class Coroutine<void> {
  using PromiseCore_ = PromiseCore<void>;
  using SharedCore_ = PromiseCore_ *;

public:
  Coroutine() = default;
  // Coroutine is pinned in memory and not allowed to copy/move.
  Coroutine(Coroutine<void> &&other) noexcept = delete;
  Coroutine &operator=(const Coroutine<void> &other) = delete;
  Coroutine &operator=(Coroutine<void> &&other) = delete;
  Coroutine(const Coroutine<void> &other) = delete;
  ~Coroutine() = default;

  /// Part of the coroutine protocol.
  Promise<void> get_return_object() { return Promise<void>{core_}; }
  /// Part of the coroutine protocol: `uvco` coroutines always run until the
  /// first suspension point.
  std::suspend_never initial_suspend() noexcept {
    core_.setRunning(
        std::coroutine_handle<Coroutine<void>>::from_promise(*this));
    return {};
  }
  /// Part of the coroutine protocol: the coroutine stays alive until the
  /// corresponding Promise<void> is destroyed. Only then can the coroutine and
  /// its core be released.
  ///
  /// Similarly, when the promise is dropped, the coroutine is destroyed.
  std::suspend_always final_suspend() noexcept { return {}; }

  /// Part of the coroutine protocol: resumes an awaiting coroutine, if there
  /// is one.
  void return_void();
  /// Part of the coroutine protocol: store exception in core and resume
  /// awaiting coroutine.
  void unhandled_exception();

private:
  PromiseCore_ core_;
};

/// @}

} // namespace uvco
