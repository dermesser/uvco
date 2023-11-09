#pragma once

#include "internal_util.h"

#include <cassert>
#include <coroutine>
#include <fmt/format.h>
#include <functional>
#include <optional>
#include <type_traits>

namespace uvco {

template <typename T> class PromiseCore {
public:
  std::optional<T> slot;
  void set_resume(std::coroutine_handle<> h) { resume_ = h; }
  bool has_resume() { return resume_.has_value(); }
  void resume() {
    if (resume_) {
      resume_->resume();
      resume_.reset();
    }
  }
  ~PromiseCore() {
    // This only happens if the awaiting coroutine has never been resumed, but
    // the last promise provided by it is gone.
    if (resume_)
      resume_->destroy();
  }

private:
  std::optional<std::coroutine_handle<>> resume_;
};

template <> class PromiseCore<void> {
public:
  bool ready = false;
  void set_resume(std::coroutine_handle<> h) { resume_ = h; }
  bool has_resume() { return resume_.has_value(); }
  void resume() {
    if (resume_) {
      resume_->resume();
      resume_.reset();
    }
  }
  ~PromiseCore() {
    if (resume_)
      resume_->destroy();
  }

private:
  std::optional<std::coroutine_handle<>> resume_;
};

template <typename T> class Promise : public LifetimeTracker<Promise<T>> {
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
    bool await_suspend(std::coroutine_handle<> handle) {
      if (core_->has_resume()) {
        fmt::print("promise is already being waited on!\n");
        assert(false);
      }
      core_->set_resume(handle);
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

template <typename T> struct MultiPromiseCore {
  std::optional<std::coroutine_handle<>> resume;
  std::optional<T> slot;
};

template <> struct MultiPromiseCore<void> {
  std::optional<std::coroutine_handle<>> resume;
  bool ready = false;
};

template <typename T>
class MultiPromise : public LifetimeTracker<MultiPromise<T>> {
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
    core_->resume->resume();
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
    assert(core_->resume);
    if (core_->resume) {
      // TODO: schedule resume on event loop.
      core_->resume->resume();
    }
    return std::suspend_never{};
  }

  MultiPromiseAwaiter_ operator co_await() {
    return MultiPromiseAwaiter_{core_};
  }

  bool ready() { return core_->slot.has_value(); }
  T result() { return std::move(*core_->slot); }

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
      core_->resume = handle;
      return true;
    }
    std::optional<T> await_resume() {
      std::optional<T> result = std::move(core_->slot);
      core_->slot.reset();
      // Obvious - but important to avoid constantly yielding!
      assert(!core_->slot);
      return std::move(result);
    }

    SharedCore_ core_;
  };

  SharedCore_ core_;
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
      if (core_->has_resume()) {
        fmt::print("promise is already being waited on!\n");
        assert(false);
      }
      core_->set_resume(handle);
      return true;
    }
    void await_resume() {}

    std::shared_ptr<PromiseCore<void>> core_;
  };
  std::shared_ptr<PromiseCore<void>> core_;
};

} // namespace uvco
