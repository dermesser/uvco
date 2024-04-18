
#pragma once

#include "exception.h"
#include "promise.h"
#include "promise/promise_core.h"

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

  /// See `PromiseCore::set_handle`. In contrast, a finished multipromise core
  /// can be reset to the waiting state, in order to yield the next value.
  void set_handle(std::coroutine_handle<> handle) override {
    // Once an external scheduler works, Promises will not be nested anymore
    // (resume called by resume down in the stack)
    //
    // BOOST_ASSERT(PromiseCore<T>::state_
    // == PromiseState::init || PromiseCore<T>::state_ ==
    // PromiseState::finished);
    //
    // state is init or running (latter can occur if set_handle is called from a
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
  void return_void() { core_->resume(); }

  /// Part of the coroutine protocol (see `Promise`).
  // Note: if suspend_always is chosen, we can better control when the
  // MultiPromise will be scheduled.
  std::suspend_never initial_suspend() noexcept { return {}; }
  /// Part of the coroutine protocol (see `Promise`).
  std::suspend_never final_suspend() noexcept { return {}; }

  /// Part of the coroutine protocol (see `Promise`).
  void unhandled_exception() {
    core_->slot = std::current_exception();
    core_->resume();
  }

  /// Yield a value to the calling (awaiting) coroutine.
  ///
  /// Equivalent to `co_yield = co_await promise.yield_value()`; doesn't suspend
  /// the yielding coroutine, thus it must yield using a different promise or
  /// awaiter, in order to give the calling coroutine a chance to process the
  /// event. This is purely due to the (lack of a) scheduling algorithm in
  /// `uvco`.
  ///
  /// TODO: this should suspend using a special awaiter which will return
  /// control to the generator routine once the "receiver" has read its value.
  /// Currently, this relies on another suspension point after the yield giving
  /// back control to the event loop (and thus the receiver). Therefore you
  /// can't use it directly to implement generator functions without further I/O
  /// between yielded values.
  std::suspend_never yield_value(T &&value) {
    BOOST_ASSERT(!core_->slot);
    core_->slot = std::move(value);
    core_->resume();
    return {};
  }

  std::suspend_never yield_value(const T &value) {
    BOOST_ASSERT(!core_->slot);
    core_->slot = value;
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
    constexpr explicit MultiPromiseAwaiter_(SharedCore_ core)
        : core_{std::move(core)} {}
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
      core_->set_handle(handle);
      return true;
    }
    /// Part of the coroutine protocol. Returns a value if `co_yield` was called
    /// in the generating coroutine. Otherwise, returns an empty `optional` if
    /// the generating coroutine has `co_return`ed.
    std::optional<T> await_resume() {
      if (!core_->slot) {
        return std::nullopt;
      } else {
        switch (core_->slot->index()) {
        case 0: {
          std::optional<T> result = std::move(std::get<0>(core_->slot.value()));
          core_->slot.reset();
          return std::move(result);
        }
        case 1:
          std::rethrow_exception(std::get<1>(core_->slot.value()));
        default:
          throw UvcoException(
              "MultiPromiseAwaiter_::await_resume: invalid slot");
        }
      }
    }

    SharedCore_ core_;
  };

  SharedCore_ core_;
};

/// @}

} // namespace uvco
