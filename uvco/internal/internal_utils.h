// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <boost/assert.hpp>
#include <fmt/core.h>
#include <string_view>
#include <uv.h>
#include <uv/unix.h>

#include <concepts>
#include <cstddef>
#include <string>
#include <utility>

namespace uvco {

/// @addtogroup Internal Utilities
/// @{

/// Result of a libuv operation, an errno error code.
using uv_status = int;

void allocator(uv_handle_t * /*unused*/, size_t sugg, uv_buf_t *buf);

void freeUvBuf(const uv_buf_t *buf);

// Checks if string_view is null-terminated. If it is, the fast path  is taken.
// Otherwise, a string is allocated which is null-terminated, and the function
// is called with it.
template <typename R, typename F>
  requires std::is_invocable_r_v<R, F, std::string_view>
R callWithNullTerminated(std::string_view view, F &&f) {
  if (view.data()[view.size()] == '\0') {
    return f(view.data());
  }
  std::string str(view);
  return f(str.c_str());
}

template <typename Into, typename Handle> Into *getData(const Handle *handle) {
  const void *data = uv_handle_get_data((const uv_handle_t *)handle);
  BOOST_ASSERT(nullptr != data);
  return (Into *)data;
}

template <typename Into, typename Request>
Into *getRequestData(const Request *req) {
  const void *data = uv_req_get_data((const uv_req_t *)req);
  BOOST_ASSERT(nullptr != data);
  return (Into *)data;
}

template <typename Handle, typename Data>
void setData(Handle *handle, Data *data) {
  BOOST_ASSERT(handle != nullptr);
  uv_handle_set_data((uv_handle_t *)handle, (void *)data);
}

template <typename Request, typename Data>
void setRequestData(Request *req, Data *data) {
  BOOST_ASSERT(req != nullptr);
  uv_req_set_data((uv_req_t *)req, (void *)data);
}

template <typename Handle> bool dataIsNull(Handle *handle) {
  return nullptr == uv_handle_get_data((const uv_handle_t *)handle);
}

template <typename Request> bool requestDataIsNull(Request *req) {
  return nullptr == uv_req_get_data((const uv_req_t *)req);
}

/// A polymorphic functor for deleting a `uv_handle_t`. It dispatches
/// to the correct `uv_...` function based on the handle's type. This
/// is necessary for classes like StreamBase which contain a pointer
/// to a uv_handle_t, but don't know the exact type of the handle.
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
  if constexpr (sizeof...(args) == 0) {
    return new T{};
  } else {
    return new T{std::forward<Args...>(args...)};
  }
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

template <typename T> class ZeroAtExit {
public:
  explicit ZeroAtExit(T **pointer) : ptr_{pointer} {}
  ZeroAtExit(const ZeroAtExit &) = delete;
  ZeroAtExit(ZeroAtExit &&) = delete;
  ZeroAtExit &operator=(const ZeroAtExit &) = delete;
  ZeroAtExit &operator=(ZeroAtExit &&) = delete;
  ~ZeroAtExit() {
    if (ptr_ != nullptr) {
      *ptr_ = nullptr;
    }
  }

private:
  T **ptr_ = nullptr;
};

/// @}

} // namespace uvco
