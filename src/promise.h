#pragma once

#include "internal_util.h"

#include <cassert>
#include <coroutine>
#include <fmt/format.h>
#include <functional>
#include <optional>
#include <type_traits>

namespace uvco {

template <typename T> struct PromiseCore {
  std::optional<std::coroutine_handle<>> resume;
  std::optional<T> slot;
};

template <> struct PromiseCore<void> {
  std::optional<std::coroutine_handle<>> resume;
  bool ready = false;
};

template <typename T>
class PromiseBase : public LifetimeTracker<PromiseBase<T>> {
protected:
  struct PromiseAwaiter_;
  using SharedCore_ = std::shared_ptr<PromiseCore<T>>;

public:
  PromiseBase() : core_{std::make_shared<PromiseCore<T>>()} {}
  PromiseBase(PromiseBase<T> &&) noexcept = default;
  PromiseBase &operator=(const PromiseBase<T> &) = default;
  PromiseBase &operator=(PromiseBase<T> &&) noexcept = default;
  PromiseBase(const PromiseBase<T> &other) = default;
  ~PromiseBase() = default;

  // Note: if suspend_always is chosen, we can better control when the promise
  // will be scheduled.
  std::suspend_never initial_suspend() noexcept { return {}; }
  std::suspend_never final_suspend() noexcept { return {}; }

  void unhandled_exception() { fmt::print("UNHANDLED EXCEPTION!\n"); }

  PromiseAwaiter_ operator co_await() { return PromiseAwaiter_{core_}; }

  bool ready() { return core_->slot.has_value(); }
  T result() { return std::move(*core_->slot); }

protected:
  struct PromiseAwaiter_ {
    explicit PromiseAwaiter_(SharedCore_ core) : core_{std::move(core)} {}
    PromiseAwaiter_(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_(const PromiseAwaiter_ &) = delete;
    PromiseAwaiter_ &operator=(PromiseAwaiter_ &&) = delete;
    PromiseAwaiter_ &operator=(const PromiseAwaiter_ &) = delete;
    ~PromiseAwaiter_() = default;

    bool await_ready() const { return core_->slot.has_value(); }
    virtual bool await_suspend(std::coroutine_handle<> handle) {
      if (core_->resume) {
        fmt::print("promise is already being waited on!\n");
        assert(false);
      }
      core_->resume = handle;
      return true;
    }
    T await_resume() {
      auto result = std::move(core_->slot.value());
      core_->slot.reset();
      return std::move(result);
    }

    SharedCore_ core_;
  };

  SharedCore_ core_;
};

template <typename T> class Promise : public PromiseBase<T> {
public:
  using promise_type = Promise<T>;

  Promise<T> get_return_object() { return *this; }

  void return_value(T &&value) {
    auto &core_ = PromiseBase<T>::core_;
    assert(!core_->slot);
    core_->slot = std::move(value);
    // TODO: don't resume immediately, but schedule resumption. The promise is
    // only destroyed after resume() returns, this has the promise hang around
    // longer than needed.
    if (core_->resume) {
      core_->resume->resume();
    }
  }
};

template <typename T> class MultiPromise : public PromiseBase<T> {
  struct MultiPromiseAwaiter_;

public:
  using promise_type = MultiPromise<T>;

  static_assert(!std::is_void_v<T>);

  MultiPromise() = default;

  MultiPromise<T> get_return_object() { return *this; }

  void return_void() {
    auto &core_ = PromiseBase<T>::core_;
    if (core_->resume) {
      core_->resume->resume();
    }
  }

  MultiPromiseAwaiter_ operator co_await() {
    return MultiPromiseAwaiter_{PromiseBase<T>::core_};
  }

  // co_yield = co_await promise.yield_value()
  std::suspend_never yield_value(T &&value) {
    // yield_awaiter is returned to the yielding coroutine.
    auto &core_ = PromiseBase<T>::core_;
    assert(!core_->slot);
    core_->slot = std::move(value);
    assert(core_->resume);
    // Resume the waiting coroutine until the next call to co_await.
    if (core_->resume) {
      // TODO: schedule resumption instead of resuming in-line. This will lead
      // to a growing stack!
      fmt::print("before resume (yield)\n");
      core_->resume->resume();
      fmt::print("after resume (yield)\n");
    }
    return std::suspend_never{};
  }

private:
  struct MultiPromiseAwaiter_ : public PromiseBase<T>::PromiseAwaiter_ {
    explicit MultiPromiseAwaiter_(PromiseBase<T>::SharedCore_ core)
        : PromiseBase<T>::PromiseAwaiter_{std::move(core)} {}

    bool await_suspend(std::coroutine_handle<> handle) override {
      auto &core_ = PromiseBase<T>::PromiseAwaiter_::core_;
      core_->resume = handle;
      return true;
    }
    std::optional<T> await_resume() {
      auto &core_ = PromiseBase<T>::PromiseAwaiter_::core_;
      auto result = std::move(core_->slot);
      core_->slot.reset();
      return std::move(result);
    }
  };
};

template <> class Promise<void> : public LifetimeTracker<Promise<void>> {
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
    if (core_->resume) {
      core_->resume->resume();
    }
  }
  void unhandled_exception() { fmt::print("UNHANDLED EXCEPTION!\n"); }

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
      if (core_->resume) {
        fmt::print("promise is already being waited on!\n");
        assert(false);
      }
      core_->resume = handle;
      return true;
    }
    void await_resume() {}

    std::shared_ptr<PromiseCore<void>> core_;
  };
  std::shared_ptr<PromiseCore<void>> core_;
};

} // namespace uvco
