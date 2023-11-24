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

enum class PromiseState {
  init = 0,
  waitedOn = 1,
  running = 2,
  finished = 3,
};

template <typename T>
class PromiseCore {
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

template <>
class PromiseCore<void> {
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

template <typename T> class Promise {
protected:
  struct PromiseAwaiter_;
  using PromiseCore_ = PromiseCore<T>;
  using SharedCore_ = std::shared_ptr<PromiseCore_>;

public:
  using promise_type = Promise<T>;

  Promise() : core_{std::make_shared<PromiseCore_>()} {}
  explicit Promise(T &&result)
      : core_{std::make_shared<PromiseCore_>(std::move(result))} {}
  Promise(Promise<T> &&) noexcept = default;
  Promise &operator=(const Promise<T> &) = default;
  Promise &operator=(Promise<T> &&) noexcept = default;
  Promise(const Promise<T> &other) = default;
  ~Promise() = default;

  Promise<T> get_return_object() { return *this; }

  void return_value(T &&value) {
    auto &core_ = Promise<T>::core_;
    BOOST_ASSERT(!core_->slot);
    core_->slot = std::move(value);
    // TODO: don't resume immediately, but schedule resumption. The promise is
    // only destroyed after resume() returns, this has the promise hang around
    // longer than needed.
    core_->resume();
  }
  // Note: if suspend_always is chosen, we can better control when the promise
  // will be scheduled.
  std::suspend_never initial_suspend() noexcept { return {}; }
  std::suspend_never final_suspend() noexcept { return {}; }

  void unhandled_exception() {
    std::rethrow_exception(std::current_exception());
  }

  PromiseAwaiter_ operator co_await() { return PromiseAwaiter_{core_}; }

  bool ready() { return core_->slot.has_value(); }
  T result() {
    T result = std::move(*core_->slot);
    core_.reset();
    return result;
  }

protected:
  struct PromiseAwaiter_ {
    explicit PromiseAwaiter_(SharedCore_ core) : core_{std::move(core)} {}
    PromiseAwaiter_(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_(const PromiseAwaiter_ &) = delete;
    PromiseAwaiter_ &operator=(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_ &operator=(const PromiseAwaiter_ &) = delete;
    ~PromiseAwaiter_() = default;

    bool await_ready() const { return core_->slot.has_value(); }
    bool await_suspend(std::coroutine_handle<> handle) {
      BOOST_ASSERT_MSG(!core_->willResume(),
                       "promise is already being waited on!\n");
      core_->set_resume(handle);
      return true;
    }
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

template <> class Promise<void> {
  struct PromiseAwaiter_;
  using SharedCore_ = std::shared_ptr<PromiseCore<void>>;

public:
  using promise_type = Promise<void>;

  Promise() : core_{std::make_shared<PromiseCore<void>>()} {}
  Promise(Promise<void> &&) noexcept = default;
  Promise &operator=(const Promise<void> &) = default;
  Promise &operator=(Promise<void> &&) noexcept = default;
  Promise(const Promise<void> &other) = default;
  ~Promise() {}

  // Construct a Promise<void> that is immediately ready.
  static Promise<void> immediate();

  Promise<void> get_return_object() { return *this; }
  std::suspend_never initial_suspend() noexcept { return {}; }
  std::suspend_never final_suspend() noexcept { return {}; }

  void return_void();
  void unhandled_exception();

  PromiseAwaiter_ operator co_await() { return PromiseAwaiter_{core_}; }

  bool ready() { return core_->ready; }
  void result() {}

private:
  struct PromiseAwaiter_ {
    explicit PromiseAwaiter_(SharedCore_ core) : core_{std::move(core)} {}
    PromiseAwaiter_(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_(const PromiseAwaiter_ &) = delete;
    PromiseAwaiter_ &operator=(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_ &operator=(const PromiseAwaiter_ &) = delete;

    bool await_ready() const;
    bool await_suspend(std::coroutine_handle<> handle);
    void await_resume();

    std::shared_ptr<PromiseCore<void>> core_;
  };
  std::shared_ptr<PromiseCore<void>> core_;
};

template <typename T> class MultiPromise {
protected:
  struct MultiPromiseAwaiter_;
  using PromiseCore_ = MultiPromiseCore<T>;
  using SharedCore_ = std::shared_ptr<PromiseCore_>;

public:
  using promise_type = MultiPromise<T>;
  static_assert(!std::is_void_v<T>);

  MultiPromise() : core_{std::make_shared<PromiseCore_>()} {}
  MultiPromise(MultiPromise<T> &&) noexcept = default;
  MultiPromise &operator=(const MultiPromise<T> &) = default;
  MultiPromise &operator=(MultiPromise<T> &&) noexcept = default;
  MultiPromise(const MultiPromise<T> &other) = default;
  ~MultiPromise() = default;

  MultiPromise<T> get_return_object() { return *this; }

  void return_void() {
    // TODO: don't resume immediately, but schedule resumption. The MultiPromise
    // is only destroyed after resume() returns, this has the MultiPromise hang
    // around longer than needed.
    core_->resume();
  }
  // Note: if suspend_always is chosen, we can better control when the
  // MultiPromise will be scheduled.
  std::suspend_never initial_suspend() noexcept { return {}; }
  std::suspend_never final_suspend() noexcept { return {}; }

  void unhandled_exception() {
    std::rethrow_exception(std::current_exception());
  }

  // co_yield = co_await promise.yield_value()
  std::suspend_never yield_value(T &&value) {
    BOOST_ASSERT(!core_->slot);
    core_->slot = std::move(value);
    // TODO: schedule resume on event loop.
    core_->resume();
    return {};
  }

  MultiPromiseAwaiter_ operator co_await() {
    return MultiPromiseAwaiter_{core_};
  }

  bool ready() { return core_->slot.has_value(); }
  T result() {
    T result = std::move(*core_->slot);
    core_->slot.reset();
    return result;
  }

protected:
  struct MultiPromiseAwaiter_ {
    explicit MultiPromiseAwaiter_(SharedCore_ core) : core_{std::move(core)} {}
    MultiPromiseAwaiter_(MultiPromiseAwaiter_ &&) = delete;
    MultiPromiseAwaiter_(const MultiPromiseAwaiter_ &) = delete;
    MultiPromiseAwaiter_ &operator=(MultiPromiseAwaiter_ &&) = delete;
    MultiPromiseAwaiter_ &operator=(const MultiPromiseAwaiter_ &) = delete;
    ~MultiPromiseAwaiter_() = default;

    bool await_ready() const { return core_->slot.has_value(); }
    virtual bool await_suspend(std::coroutine_handle<> handle) {
      BOOST_ASSERT_MSG(!core_->willResume(),
                       "promise is already being waited on!\n");
      core_->set_resume(handle);
      return true;
    }
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
