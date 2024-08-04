
#pragma once

#include "uvco/exception.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/promise.h"
#include "uvco/promise/promise_core.h"

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>

namespace uvco {

/// @addtogroup Promise
/// @{

/// A `MultiPromiseCore` works like a `PromiseCore`, but with an adapted state
/// machine: it can transition from `finished` back to `waitedOn`, and therefore
/// yield more than one value. It is used by `MultiPromise`, a
/// generator-like type.
template <typename T> class MultiPromiseCore : public PromiseCore<T> {
public:
  MultiPromiseCore() = default;
  MultiPromiseCore(const MultiPromiseCore &) = delete;
  MultiPromiseCore(MultiPromiseCore &&) = delete;
  MultiPromiseCore &operator=(const MultiPromiseCore &) = delete;
  MultiPromiseCore &operator=(MultiPromiseCore &&) = delete;
  static_assert(!std::is_void_v<T>);
  ~MultiPromiseCore() override = default;

  /// See `PromiseCore::setHandle`. Called by a `MultiPromise` when it is
  /// `co_await`ed.
  ///
  /// In contrast to a `PromiseCore`, a finished
  /// multipromise core can be reset to the waiting state, in order to yield the
  /// next value, when the MultiPromise is `co_await`ed again.
  void setHandle(std::coroutine_handle<> handle) override {
    // Once an external scheduler works, Promises will not be nested anymore
    // (resume called by resume down in the stack)
    //
    // BOOST_ASSERT(PromiseCore<T>::state_
    // == PromiseState::init || PromiseCore<T>::state_ ==
    // PromiseState::finished);
    //
    // state is init or running (latter can occur if setHandle is called from a
    // stack originating at resume()).
    BOOST_ASSERT_MSG(PromiseCore<T>::state_ != PromiseState::waitedOn,
                     "MultiPromise must be co_awaited before next yield");
    BOOST_ASSERT_MSG(!PromiseCore<T>::handle_,
                     "MultiPromise must be co_awaited before next yield");
    PromiseCore<T>::handle_ = handle;
    PromiseCore<T>::state_ = PromiseState::waitedOn;
  }

  /// See `Promise::resume`. Implemented here to provide a distinction in stack
  /// traces.
  void resume() override { PromiseCore<T>::resume(); }

  /// Resume the generator from its last `co_yield` point, so it can yield the
  /// next value.
  ///
  /// Called by a `MultiPromiseAwaiter_` when the `MultiPromise` is
  /// `co_await`ed, after a value has been successfully awaited.
  void resumeGenerator() {
    BOOST_ASSERT(PromiseCore<T>::state_ != PromiseState::finished);
    if (generatorHandle_) {
      const auto generatorHandle = generatorHandle_.value();
      generatorHandle_.reset();
      Loop::enqueue(generatorHandle);
    }
  }

  /// Suspend the generator coroutine at the current `co_yield` point by storing
  /// the handle for a later resumption.
  ///
  /// Called by the `MultiPromise<...>::yield_value` method when `co_yield` is
  /// invoked inside the generator.
  void suspendGenerator(std::coroutine_handle<> handle) {
    BOOST_ASSERT_MSG(!generatorHandle_,
                     "MultiPromiseCore::suspendGenerator: generatorHandle_ is "
                     "already set");
    generatorHandle_ = handle;
  }

  /// Cancel the generator coroutine. This will drop all stack variables inside
  /// the generator (and run their destructors), and ensure that the
  /// generatorHandle_ will never resume from the currently yielded value.
  ///
  /// Called by tje `MultiPromise` destructor and `MultiPromise::cancel()`.
  void cancelGenerator() {
    terminated();
    if (generatorHandle_) {
      const std::coroutine_handle<> generatorHandle = generatorHandle_.value();
      generatorHandle_.reset();
      // Careful: within this function, this class' dtor is called!
      generatorHandle.destroy();
    }
  }

  /// Mark generator as finished, yielding no more values. Called from within
  /// the MultiPromise upon return_value and unhandled_exception. From hereon
  /// awaiting the generator will either rethrow the thrown exception, or yield
  /// nullopt.
  void terminated() { terminated_ = true; }

  /// Check if the generator has been cancelled or has returned.
  [[nodiscard]] bool isTerminated() const { return terminated_; }

private:
  /// Coroutine handle referring to the suspended generator.
  std::optional<std::coroutine_handle<>> generatorHandle_;
  /// Set to true once the generator coroutine has returned or has been
  /// cancelled.
  bool terminated_ = false;
};

template <typename T> class Generator;

/// A `MultiPromise` is like a `Promise`, except that it can resolve more than
/// just once. A coroutine returning a `MultiPromise` (called a "generator
/// coroutine") uses `co_yield` to return values to the awaiting
/// coroutine. For example in the `Udp` class, a method exists which generates
/// packets.
///
/// As an adapter, for example for SelectSet, the `next()` method returns a
/// promise returning the next yielded value.
///
/// NOTE: currently uses a `shared_ptr` PromiseCore, due to issues with
/// inheritance. It is expected that normal `Promise<T>` will be used most
/// frequently, therefore the lack of optimization is not as grave.
template <typename T> class MultiPromise {
protected:
  struct MultiPromiseAwaiter_;

  using PromiseCore_ = MultiPromiseCore<T>;
  using SharedCore_ = std::shared_ptr<PromiseCore_>;

public:
  /// Part of the coroutine protocol: the `MultiPromise` is both return type and
  /// promise type.
  using promise_type = Generator<T>;
  /// It doesn't make sense to yield void. For that, just ues a normal coroutine
  /// (`Promise<void>`) and repeatedly call it.
  static_assert(!std::is_void_v<T>);

  /// An unfulfilled `MultiPromise`.
  MultiPromise(MultiPromise<T> &&) noexcept = default;
  MultiPromise &operator=(const MultiPromise<T> &) = delete;
  MultiPromise &operator=(MultiPromise<T> &&) noexcept = delete;
  MultiPromise(const MultiPromise<T> &other) = default;
  ~MultiPromise() {
    // Us and the coroutine frame; but we're about to be destroyed
    if (core_.use_count() == 2) {
      // -> cancel generator. Because the coroutine frame keeps a reference to
      // the core, we can't do this in the core's destructor.
      PromiseCore_ *weakCore = core_.get();
      core_.reset();
      // The core's dtor is called while cancelGenerator runs.
      weakCore->cancelGenerator();
    }
  }

  /// Obtain the next value yielded by a generator coroutine. This is less
  /// efficient than awaiting the multipromise directly.
  Promise<std::optional<T>> next() { co_return (co_await *this); }

  /// Return an awaiter for this MultiPromise, which resumes the waiting
  /// coroutine once the generator yields its next value.
  ///
  /// Used when `co_await`ing a MultiPromise created by a generator coroutine.
  /// The awaiter handles the actual suspension and resumption.
  MultiPromiseAwaiter_ operator co_await() const {
    BOOST_ASSERT(core_);
    return MultiPromiseAwaiter_{core_};
  }

  /// Returns true if a value is available, or the generator has returned or
  /// thrown.
  bool ready() { return core_->slot.has_value() || core_->isTerminated(); }

  /// Immediately cancel the suspended generator coroutine. This will drop all
  /// stack variables inside the generator (and run their destructors), and
  /// ensure that the generator will never resume from the currently yielded
  /// value.
  ///
  /// `cancel()` is called automatically once the (last) MultiPromise instance
  /// referring to a coroutine is destroyed.
  ///
  /// (This can be solved by distinguishing between the returned object and
  /// the promise object, which may be part of a future refactor. In that case,
  /// the generator's promise object should only contain a weak reference to
  /// this MultiPromiseCore, and the caller contains a strong reference. The
  /// core's destructor cancels the generator once the caller doesn't require
  /// it anymore.)
  void cancel() { core_->cancelGenerator(); }

protected:
  /// A `MultiPromiseAwaiter_` handles suspension and resumption of coroutines
  /// receiving values from a generating (yielding) coroutine. This awaiter is
  /// used when applying the `co_await` operator on a `MultiPromise`.
  struct MultiPromiseAwaiter_ {
    constexpr explicit MultiPromiseAwaiter_(SharedCore_ core)
        : core_{std::move(core)} {}
    MultiPromiseAwaiter_(MultiPromiseAwaiter_ &&) = delete;
    MultiPromiseAwaiter_(const MultiPromiseAwaiter_ &) = delete;
    MultiPromiseAwaiter_ &operator=(MultiPromiseAwaiter_ &&) = delete;
    MultiPromiseAwaiter_ &operator=(const MultiPromiseAwaiter_ &) = delete;
    ~MultiPromiseAwaiter_() = default;

    /// Part of the coroutine protocol. Returns `true` if the MultiPromise
    /// already has a value.
    [[nodiscard]] bool await_ready() const {
      return core_->isTerminated() || core_->slot.has_value();
    }
    /// Part of the coroutine protocol. Always returns `true`; stores the
    /// suspension handle in the MultiPromiseCore for later resumption.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) const {
      BOOST_ASSERT_MSG(!core_->willResume(),
                       "promise is already being waited on!\n");
      core_->setHandle(handle);
      core_->resumeGenerator();
      return true;
    }
    /// Part of the coroutine protocol. Returns a value if `co_yield` was called
    /// in the generating coroutine. Otherwise, returns an empty `optional` if
    /// the generating coroutine has `co_return`ed.
    ///
    /// If an exception has been thrown, repeatedly rethrows this exception upon
    /// awaiting.
    std::optional<T> await_resume() const {
      if (!core_->slot) {
        // Terminated by co_return
        return std::nullopt;
      }
      switch (core_->slot->index()) {
      [[likely]] case 0: {
        std::optional<T> result = std::move(std::get<0>(core_->slot.value()));
        core_->slot.reset();
        return std::move(result);
      }
      case 1:
        // Terminated by exception
        BOOST_ASSERT(core_->isTerminated());
        std::rethrow_exception(std::get<1>(core_->slot.value()));
      default:
        throw UvcoException("MultiPromiseAwaiter_::await_resume: invalid slot");
      }
    }

    SharedCore_ core_;
  };

  template <typename U> friend class Generator;

  explicit MultiPromise(SharedCore_ core) : core_{std::move(core)} {}

  SharedCore_ core_;
};

/// Generator is the promise object type for generator-type coroutines (those
/// returning a `MultiPromise`).
template <typename T> class Generator {
  struct YieldAwaiter_;
  using PromiseCore_ = MultiPromiseCore<T>;
  using SharedCore_ = std::shared_ptr<PromiseCore_>;

public:
  // A generator promise object is pinned in memory (copy/move forbidden).
  Generator() : core_{std::make_shared<PromiseCore_>()} {}
  Generator(Generator<T> &&) noexcept = delete;
  Generator &operator=(const Generator<T> &) = delete;
  Generator &operator=(Generator<T> &&) noexcept = delete;
  Generator(const Generator<T> &other) = delete;
  ~Generator() = default;

  /// A generator (yielding) coroutine returns a MultiPromise.
  MultiPromise<T> get_return_object() { return MultiPromise<T>{core_}; }

  /// A MultiPromise coroutine ultimately returns void. This is signaled to the
  /// caller by returning an empty `std::optional`.
  void return_void() {
    core_->terminated();
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
    core_->slot = std::current_exception();
    core_->terminated();
    core_->resume();
  }

  /// Yield a value to the calling (awaiting) coroutine.
  ///
  /// Equivalent to `co_yield = co_await promise.yield_value()` (defined in C++
  /// standard); suspends the generator coroutine and resumes the awaiting
  /// coroutine if there is one.
  ///
  /// If nobody is awaiting a value from this generator, the yielded value is
  /// still moved into the generator's slot, but the generator is not resumed.
  /// Upon the next `co_await`, the returned `MultiPromiseAwaiter_` will
  /// immediately return the value without resuming the generator.
  YieldAwaiter_ yield_value(T &&value) {
    BOOST_ASSERT(!core_->slot);
    core_->slot = std::move(value);
    core_->resume();
    return YieldAwaiter_{*core_};
  }

  YieldAwaiter_ yield_value(const T &value) {
    BOOST_ASSERT(!core_->slot);
    core_->slot = value;
    core_->resume();
    return YieldAwaiter_{*core_};
  }

private:
  /// A `YieldAwaiter_` suspends a coroutine returning a MultiPromise (i.e. a
  /// generator) and resumes it when the value is read by the awaiting
  /// coroutine.
  struct YieldAwaiter_ {
    explicit YieldAwaiter_(PromiseCore_ &core) : core_{core} {}

    [[nodiscard]] bool await_ready() const { return !core_.slot.has_value(); }

    bool await_suspend(std::coroutine_handle<> handle) {
      core_.suspendGenerator(handle);
      return true;
    }

    void await_resume() {
      // Returning into the generator coroutine
    }

    PromiseCore_ &core_;
  };

  SharedCore_ core_;
};

/// @}

} // namespace uvco
