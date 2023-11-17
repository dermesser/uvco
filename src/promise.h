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
class PromiseCore : public LifetimeTracker<PromiseCore<T>> {
public:
  virtual void set_resume(std::coroutine_handle<> handle) {
    assert(state_ == PromiseState::init);
    resume_ = handle;
    state_ = PromiseState::waitedOn;
  }

  bool has_resume() { return resume_.has_value(); }

  virtual void resume() {
    if (resume_) {
      state_ = PromiseState::running;
      auto resume = *resume_;
      resume_.reset();
      resume.resume();
    }
    // This can happen if await_ready() is true immediately.
    state_ = PromiseState::finished;
  }

  virtual ~PromiseCore() {
    assert(state_ != PromiseState::running);
    if (state_ == PromiseState::init)
      fmt::print("value Promise destroyed without being waited on ({})\n",
                 typeid(T).name());
    // This only happens if the awaiting coroutine has never been resumed, but
    // the last promise provided by it is gone.
    // Important: we may only destroy a suspended coroutine, not a finished one:
    // co_return already destroys coroutine state.
    if (resume_)
      resume_->destroy();
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
    // assert(PromiseCore<T>::state_
    // == PromiseState::init || PromiseCore<T>::state_ ==
    // PromiseState::finished);
    PromiseCore<T>::resume_ = handle;
    PromiseCore<T>::state_ = PromiseState::waitedOn;
  }
  // For better stacktraces.
  void resume() override { PromiseCore<T>::resume(); }
};

template <>
class PromiseCore<void> : public LifetimeTracker<PromiseCore<void>> {
public:
  bool ready = false;
  void set_resume(std::coroutine_handle<> h) {
    assert(state_ == PromiseState::init);
    resume_ = h;
    state_ = PromiseState::waitedOn;
  }
  bool has_resume() { return resume_.has_value(); }
  void resume() {
    if (resume_) {
      assert(state_ == PromiseState::waitedOn);
      auto resume = *resume_;
      resume_.reset();
      state_ = PromiseState::running;
      resume();
    }
    state_ = PromiseState::finished;
  }
  ~PromiseCore() {
    assert(state_ != PromiseState::running);
    if (resume_)
      resume_->destroy();
  }

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
  Promise(Promise<T> &&) noexcept = default;
  Promise &operator=(const Promise<T> &) = default;
  Promise &operator=(Promise<T> &&) noexcept = default;
  Promise(const Promise<T> &other) = default;
  ~Promise() = default;

  Promise<T> get_return_object() { return *this; }

  void return_value(T &&value) {
    auto &core_ = Promise<T>::core_;
    assert(!core_->slot);
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
      BOOST_ASSERT_MSG(!core_->has_resume(),
                       "promise is already being waited on!\n");
      core_->set_resume(handle);
      return true;
    }
    T await_resume() {
      assert(core_->slot.has_value());
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

  Promise<void> get_return_object() { return *this; }
  std::suspend_never initial_suspend() noexcept { return {}; }
  std::suspend_never final_suspend() noexcept { return {}; }

  void return_void() {
    core_->ready = true;
    core_->resume();
  }
  void unhandled_exception() {
    std::rethrow_exception(std::current_exception());
  }

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

    bool await_ready() const { return core_->ready; }

    bool await_suspend(std::coroutine_handle<> handle) {
      BOOST_ASSERT_MSG(!core_->has_resume(),
                       "promise is already being waited on!\n");
      core_->set_resume(handle);
      return true;
    }
    void await_resume() {}

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

  MultiPromise() : core_{std::make_shared<PromiseCore_>()} {}
  MultiPromise(MultiPromise<T> &&) noexcept = default;
  MultiPromise &operator=(const MultiPromise<T> &) = default;
  MultiPromise &operator=(MultiPromise<T> &&) noexcept = default;
  MultiPromise(const MultiPromise<T> &other) = default;
  ~MultiPromise() = default;

  static_assert(!std::is_void_v<T>);

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
    assert(!core_->slot);
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

    bool await_ready() const {
      const bool ready = core_->slot.has_value();
      return ready;
    }
    virtual bool await_suspend(std::coroutine_handle<> handle) {
      BOOST_ASSERT_MSG(!core_->has_resume(),
                       "promise is already being waited on!\n");
      core_->set_resume(handle);
      return true;
    }
    std::optional<T> await_resume() {
      std::optional<T> result = std::move(core_->slot);
      core_->slot.reset();
      // Obvious - but important to avoid constantly yielding!
      assert(!core_->slot);
      return result;
    }

    SharedCore_ core_;
  };

  SharedCore_ core_;
};

} // namespace uvco
