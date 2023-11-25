// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "internal_utils.h"

#include <boost/assert.hpp>

#include <cassert>
#include <coroutine>
#include <fmt/format.h>
#include <functional>
#include <optional>
#include <type_traits>

namespace uvco {

/// Valid states for a promise to be in:
///
/// Initially, `init` marks a newly constructed promise. Once a coroutine waits
/// for its result, the promise transitions to `waitedOn`. At that point, the
/// `resume_` field contains a resume handle (of the waiter). Once the promise
/// is ready and the caller is resumed, the state transitions to `running`.
/// After the caller has been run (and suspended again), the state is
/// `finished`, and no more operations may be executed on this promise.
enum class PromiseState {
  init = 0,
  waitedOn = 1,
  running = 2,
  finished = 3,
};

/// A `PromiseCore` is shared among copies of promises waiting for the same
/// coroutine. It contains a state, a result (of type `T`), and potentially a
/// `coroutine_handle` of the coroutine waiting on it. Only one coroutine may
/// await a promise, this is enforced here.
template <typename T> class PromiseCore {
public:
  PromiseCore() = default;
  explicit PromiseCore(T &&value)
      : slot{std::move(value)}, state_{PromiseState::finished} {}

  virtual void set_resume(std::coroutine_handle<> handle) {
    BOOST_ASSERT(state_ == PromiseState::init);
    resume_ = handle;
    state_ = PromiseState::waitedOn;
  }

  bool willResume() { return resume_.has_value(); }

  virtual void resume() {
    if (resume_) {
      BOOST_ASSERT(state_ == PromiseState::waitedOn);
      state_ = PromiseState::running;
      auto resume = *resume_;
      resume_.reset();
      resume.resume();
    } else {
      // This occurs if no co_await has occured until resume. Either the promise
      // was not co_awaited, or the producing coroutine immediately returned a
      // value. (await_ready() == true)
    }

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
      // Happens in MultiPromiseCore on co_return if the co_awaiter has lost
      // interest. Harmless if !resume_ (asserted above).
      break;
    }
  }

  virtual ~PromiseCore() {
    if (state_ != PromiseState::finished) {
      fmt::print(stderr,
                 "PromiseCore destroyed without ever being resumed ({})\n",
                 typeid(T).name());
    }
    // This only happens if the awaiting coroutine has never been resumed, but
    // the last promise provided by it is gone.
    // Important: we may only destroy a suspended coroutine, not a finished one:
    // co_return already destroys coroutine state.
    if (resume_) {
      resume_->destroy();
    }
  }

  std::optional<T> slot;

protected:
  std::optional<std::coroutine_handle<>> resume_;
  PromiseState state_ = PromiseState::init;
};

/// A `MultiPromiseCore` works like a `PromiseCore`, but with an adapted state
/// machine: it can transition from `finished` back to `waitedOn`, and therefore
/// yield more than one value. Therefore it's used by `MultiPromise`, a
/// generator-like type.
template <typename T> class MultiPromiseCore : public PromiseCore<T> {
public:
  static_assert(!std::is_void_v<T>);

  ~MultiPromiseCore() override = default;

  void set_resume(std::coroutine_handle<> handle) override {
    // Once an external scheduler works, Promises will not be nested anymore
    // (resume called by resume down in the stack)
    //
    // BOOST_ASSERT(PromiseCore<T>::state_
    // == PromiseState::init || PromiseCore<T>::state_ ==
    // PromiseState::finished);
    //
    // state is init or running (latter can occur if set_resume is called from a
    // stack originating at resume()).
    BOOST_ASSERT_MSG(PromiseCore<T>::state_ != PromiseState::waitedOn,
                     "MultiPromise must be co_awaited before next yield");
    BOOST_ASSERT_MSG(!PromiseCore<T>::resume_,
                     "MultiPromise must be co_awaited before next yield");
    PromiseCore<T>::resume_ = handle;
    PromiseCore<T>::state_ = PromiseState::waitedOn;
  }
  // For better stacktraces.
  void resume() override { PromiseCore<T>::resume(); }
};

template <> class PromiseCore<void> {
public:
  PromiseCore() = default;

  bool ready = false;
  void set_resume(std::coroutine_handle<> h);
  bool willResume();
  void resume();
  ~PromiseCore();

  // Immediately marks a core as fulfilled (but does not resume); used for
  // Promise<void>::imediate().
  void immediateFulfill();

private:
  std::optional<std::coroutine_handle<>> resume_;
  PromiseState state_ = PromiseState::init;
};

/// A `Promise` is the core type of `uvco`, and returned from coroutines. A
/// coroutine is a function containing either of `co_await`, `co_yield`, or
/// `co_return`.
///
/// A Promise can be awaited on; for this the inner type `PromiseAwaiter_` is
/// used. The `PromiseCore` manages the low-level resumption while the `Promise`
/// and `PromiseAwaiter_` types fulfill the C++ standard coroutine protocol.
template <typename T> class Promise {
protected:
  struct PromiseAwaiter_;
  /// PromiseCore_ handles the inner mechanics of resumption and suspension.
  using PromiseCore_ = PromiseCore<T>;
  using SharedCore_ = std::shared_ptr<PromiseCore_>;

public:
  /// Part of the coroutine protocol: specifies which class will define the
  /// coroutine behavior. In this case, the `Promise<T>` class is both the
  /// return type and the promise type.
  ///
  /// Note that the awaiter type is separate (`PromiseAwaiter_`).
  using promise_type = Promise<T>;

  /// Unfulfilled, empty promise.
  Promise() : core_{std::make_shared<PromiseCore_>()} {}
  /// Fulfilled promise; resolves immediately.
  explicit Promise(T &&result)
      : core_{std::make_shared<PromiseCore_>(std::move(result))} {}

  Promise(Promise<T> &&) noexcept = default;
  Promise &operator=(const Promise<T> &) = default;
  Promise &operator=(Promise<T> &&) noexcept = default;
  Promise(const Promise<T> &other) = default;
  ~Promise() = default;

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
    std::rethrow_exception(std::current_exception());
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
    explicit PromiseAwaiter_(SharedCore_ core) : core_{std::move(core)} {}
    PromiseAwaiter_(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_(const PromiseAwaiter_ &) = delete;
    PromiseAwaiter_ &operator=(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_ &operator=(const PromiseAwaiter_ &) = delete;
    ~PromiseAwaiter_() = default;

    /// Part of the coroutine protocol: returns `true` if the promise is already
    /// fulfilled.
    bool await_ready() const { return core_->slot.has_value(); }
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
      BOOST_ASSERT(core_->slot.has_value());
      auto result = std::move(core_->slot.value());
      core_->slot.reset();
      return result;
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
  using SharedCore_ = std::shared_ptr<PromiseCore<void>>;

public:
  /// Part of the coroutine protocol: `Promise<void>` is both return type and
  /// promise type.
  using promise_type = Promise<void>;

  /// Promise ready to be awaited or fulfilled.
  Promise() : core_{std::make_shared<PromiseCore<void>>()} {}
  Promise(Promise<void> &&) noexcept = default;
  Promise &operator=(const Promise<void> &) = default;
  Promise &operator=(Promise<void> &&) noexcept = default;
  Promise(const Promise<void> &other) = default;
  ~Promise() = default;

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

/// A `MultiPromise` is like a `Promise`, except that it can resolve more than
/// just once. A coroutine returning a `MultiPromise` typically uses `co_yield`
/// to return values to the awaiting coroutine. It can therefore be used as
/// generator (for example in the `Udp` class, a method exists which generates
/// packets).
///
/// WARNING: Do not naively use e.g. a for loop, `co_yield`ing value after value
/// without intermittent suspensioin points. This will not work! The yield
/// operation does (currently) not suspend the yielding coroutine, and a yielded
/// value must first be fetched by the awaiting coroutine. Typically, you should
/// first `co_await` e.g. some socket operation, then `co_yield`, and repeat.
/// That way the coroutine will return upon the next suspension point and giving
/// an opportunity to the "receiving" coroutine to process the yielded value.
template <typename T> class MultiPromise {
protected:
  struct MultiPromiseAwaiter_;
  using PromiseCore_ = MultiPromiseCore<T>;
  using SharedCore_ = std::shared_ptr<PromiseCore_>;

public:
  /// Part of the coroutine protocol: the `MultiPromise` is both return type and
  /// promise type.
  using promise_type = MultiPromise<T>;
  /// It doesn't make sense to yield void. For that, just ues a normal coroutine
  /// (`Promise<void>`) and repeatedly call it.
  static_assert(!std::is_void_v<T>);

  /// An unfulfilled `MultiPromise`.
  MultiPromise() : core_{std::make_shared<PromiseCore_>()} {}
  MultiPromise(MultiPromise<T> &&) noexcept = default;
  MultiPromise &operator=(const MultiPromise<T> &) = default;
  MultiPromise &operator=(MultiPromise<T> &&) noexcept = default;
  MultiPromise(const MultiPromise<T> &other) = default;
  ~MultiPromise() = default;

  /// A generator (yielding) coroutine returns a MultiPromise.
  MultiPromise<T> get_return_object() { return *this; }

  /// A MultiPromise coroutine ultimately returns void. This is signaled to the
  /// caller by returning an empty `std::optional`.
  void return_void() {
    // TODO: don't resume immediately, but schedule resumption. The MultiPromise
    // is only destroyed after resume() returns, this has the MultiPromise hang
    // around longer than needed.
    core_->resume();
  }

  /// Part of the coroutine protocol (see `Promise`).
  // Note: if suspend_always is chosen, we can better control when the
  // MultiPromise will be scheduled.
  std::suspend_never initial_suspend() noexcept { return {}; }
  /// Part of the coroutine protocol (see `Promise`).
  std::suspend_never final_suspend() noexcept { return {}; }

  /// Part of the coroutine protocol (see `Promise`).
  void unhandled_exception() {
    std::rethrow_exception(std::current_exception());
  }

  /// Yield a value to the calling (awaiting) coroutine.
  ///
  /// Equivalent to `co_yield = co_await promise.yield_value()`; doesn't suspend
  /// the yielding coroutine, thus it must yield using a different promise or
  /// awaiter, in order to give the calling coroutine a chance to process the
  /// event. This is purely due to the (lack of a) scheduling algorithm in
  /// `uvco`.
  std::suspend_never yield_value(T &&value) {
    BOOST_ASSERT(!core_->slot);
    core_->slot = std::move(value);
    // TODO: schedule resume on event loop.
    core_->resume();
    return {};
  }

  /// Return an awaiter for this MultiPromise.
  ///
  /// Used when `co_await`ing a MultiPromise. The awaiter handles the actual
  /// suspension and resumption.
  MultiPromiseAwaiter_ operator co_await() {
    return MultiPromiseAwaiter_{core_};
  }

  /// Returns true if a value is available.
  bool ready() { return core_->slot.has_value(); }

protected:
  /// A `MultiPromiseAwaiter_` handles suspension and resumption of coroutines
  /// receiving values from a generating (yielding) coroutine. This awaiter is
  /// used when applying the `co_await` operator on a `MultiPromise`.
  struct MultiPromiseAwaiter_ {
    explicit MultiPromiseAwaiter_(SharedCore_ core) : core_{std::move(core)} {}
    MultiPromiseAwaiter_(MultiPromiseAwaiter_ &&) = delete;
    MultiPromiseAwaiter_(const MultiPromiseAwaiter_ &) = delete;
    MultiPromiseAwaiter_ &operator=(MultiPromiseAwaiter_ &&) = delete;
    MultiPromiseAwaiter_ &operator=(const MultiPromiseAwaiter_ &) = delete;
    ~MultiPromiseAwaiter_() = default;

    /// Part of the coroutine protocol. Returns `true` if the MultiPromise
    /// already has a value.
    bool await_ready() const { return core_->slot.has_value(); }
    /// Part of the coroutine protocol. Always returns `true`; stores the
    /// suspension handle in the MultiPromiseCore for later resumption.
    virtual bool await_suspend(std::coroutine_handle<> handle) {
      BOOST_ASSERT_MSG(!core_->willResume(),
                       "promise is already being waited on!\n");
      core_->set_resume(handle);
      return true;
    }
    /// Part of the coroutine protocol. Returns a value if `co_yield` was called
    /// in the generating coroutine. Otherwise, returns an empty `optional` if
    /// the generating coroutine has `co_return`ed.
    std::optional<T> await_resume() {
      std::optional<T> result = std::move(core_->slot);
      core_->slot.reset();
      // Obvious - but important to avoid constantly yielding!
      BOOST_ASSERT(!core_->slot);
      return result;
    }

    SharedCore_ core_;
  };

  SharedCore_ core_;
};

} // namespace uvco
