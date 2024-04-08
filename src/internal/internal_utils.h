// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <fmt/core.h>
#include <uv.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace uvco {

/// @addtogroup Internal Utilities
/// @{

/// Result of a libuv operation, an errno error code.
using uv_status = int;

void log(uv_loop_t *loop, std::string_view message);

void allocator(uv_handle_t * /*unused*/, size_t sugg, uv_buf_t *buf);

void freeUvBuf(const uv_buf_t *buf);

// A polymorphic functor for deleting a `uv_handle_t`. It dispatches
// to the correct `uv_...` function based on the handle's type.
struct UvHandleDeleter {
  static void del(uv_handle_t *handle);
  template <typename Handle> void operator()(Handle *handle) {
    del((uv_handle_t *)handle);
  }
};

/// `RefCounted<T>` is an intrusive refcounting approach, which reduces the
/// run-time of low-overhead high frequency promise code (such as buffered
/// channel ping-pong scenarios) by as much as 50% compared to `shared_ptr` use.
/// However, manual refcounting is required by objects owning a refcounted
/// object.
///
/// Use `makeRefCounted()` to allocate a new reference-counted object, and
/// store it as part of your class. Use `addRef()` and `delRef()` to keep track
/// of the current number of references.
///
/// This type currently doesn't work well with inheritance: only a class
/// directly inheriting from `RefCounted` can be managed. This is caused by the
/// current API approach.
template <typename T> class RefCounted {
public:
  // Assignment doesn't change count.
  RefCounted(const RefCounted &other) = default;
  RefCounted &operator=(const RefCounted &other) = default;
  RefCounted(RefCounted &&other) noexcept {}
  RefCounted &operator=(RefCounted &&other) noexcept {}
  virtual ~RefCounted() = default;

  /// Use in e.g. copy constructors, when creating a new reference to the same
  /// object.
  virtual T *addRef() {
    ++count_;
    return static_cast<T *>(this);
  }

  /// Use in e.g. destructors, when an existing pointer goes out of
  /// scope. Once the reference count has dropped to 0, the referred object will
  /// be deleted.
  virtual void delRef() {
    --count_;
    if (count_ == 0) {
      delete static_cast<T *>(this);
    }
  }

protected:
  RefCounted() = default;

private:
  size_t count_ = 1;
};

/// Create a new refcounted value. `T` must derive from `RefCounted<T>`.
template <typename T, typename... Args>
T *makeRefCounted(Args... args)
  requires std::derived_from<T, RefCounted<T>>
{
  return new T{std::forward<Args...>(args...)};
}

template <typename T>
T *makeRefCounted()
  requires std::derived_from<T, RefCounted<T>>
{
  return new T{};
}

extern const bool TRACK_LIFETIMES;

/// A utility that can be inherited from in order to log information
/// about object construction/destruction.
template <typename T> class LifetimeTracker {
public:
  explicit LifetimeTracker(std::string name = "") : id_{std::move(name)} {
    if (TRACK_LIFETIMES) {
      fmt::print("ctor {}()#{}\n", typeid(T).name(), id_);
    }
  }
  LifetimeTracker(LifetimeTracker &&) noexcept {
    if (TRACK_LIFETIMES) {
      fmt::print("ctor {}()#{}\n", typeid(T).name(), id_);
    }
  }
  LifetimeTracker &operator=(LifetimeTracker && /*unused*/) noexcept {
    if (TRACK_LIFETIMES) {
      fmt::print("operator={}()#{}\n", typeid(T).name(), id_);
    }
  }
  LifetimeTracker<T> &operator=(const LifetimeTracker<T> &other) {
    if (this == &other) {
      return *this;
    }
    if (TRACK_LIFETIMES) {
      fmt::print("operator={}({})#{}\n", typeid(T).name(), other.id_, id_);
    }
    id_ = fmt::format("{}/copy", other.id_);
  }
  LifetimeTracker(const LifetimeTracker<T> &other)
      : id_{fmt::format("{}/copy", other.id_)} {
    if (TRACK_LIFETIMES) {
      fmt::print("operator={}({})#{}\n", typeid(T).name(), other.id_, id_);
    }
  }
  ~LifetimeTracker() {
    if (TRACK_LIFETIMES) {
      fmt::print("dtor ~{}()\n", typeid(T).name());
    }
  }

protected:
  std::string id_;
};

/// Assert that an operation is concurrently running only one at a time,
/// by setting a flag in the constructor and unsetting it in the destructor.
class FlagGuard {
public:
  FlagGuard(const FlagGuard &) = delete;
  FlagGuard(FlagGuard &&) = delete;
  FlagGuard &operator=(const FlagGuard &) = delete;
  FlagGuard &operator=(FlagGuard &&) = delete;

  /// Uses Boost assert to ensure that the flag is not already set.
  explicit FlagGuard(bool &flag);
  ~FlagGuard();

private:
  bool &flag_;
};

/// @}

} // namespace uvco
