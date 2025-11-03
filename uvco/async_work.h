// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <exception>
#include <functional>
#include <utility>
#include <uv.h>

#include "uvco/loop/loop.h"
#include "uvco/promise/promise.h"

#include <optional>
#include <uv/unix.h>
#include <variant>

namespace uvco {

/// @addtogroup threadpool
/// @{

/// Do not use; instead, usetubmitWork<void>().
Promise<void> innerSubmitWork(const Loop &loop, std::function<void()> work);

/// Submit a function to be run on the libuv threadpool. The promise will return
/// the function's return value.
template <typename R>
  requires std::is_void_v<R> || std::is_move_constructible_v<R>
Promise<R> submitWork(const Loop &loop, std::function<R()> work) {
  std::optional<std::variant<R, std::exception_ptr>> result;
  // Erase return type and use generic submitWork().
  std::function<void()> agnosticWork = [&result, work]() {
    try {
      result = work();
    } catch (...) {
      result = std::current_exception();
    }
  };
  co_await innerSubmitWork(loop, std::move(agnosticWork));
  BOOST_ASSERT(result.has_value());
  if (result->index() == 1) {
    std::rethrow_exception(std::get<std::exception_ptr>(*result));
  }
  co_return std::get<R>(std::move(*result));
}

template <>
Promise<void> submitWork(const Loop &loop, std::function<void()> work);

/// Easily access per-thread data. Use `get()` and `getOrDefault()` to access
/// the stored value.
///
/// NOTE: this inherently leaks the stored objects, unless they are destroyed
/// using del(). in-place.
template <typename T> class ThreadLocalKey {
public:
  ThreadLocalKey() { uv_key_create(&key_); }
  ThreadLocalKey(const ThreadLocalKey &) = default;
  ThreadLocalKey(ThreadLocalKey &&) = default;
  ThreadLocalKey &operator=(const ThreadLocalKey &) = default;
  ThreadLocalKey &operator=(ThreadLocalKey &&) = default;
  ~ThreadLocalKey() = default;

  /// Destruct the stored value. The key will be empty afterwards, and
  /// getOrDefault() will create a new value.
  void del() {
    auto *value = uv_key_get(&key_);
    if (value != nullptr) {
      delete static_cast<T *>(value);
      uv_key_set(&key_, nullptr);
    }
  }

  /// Get the stored value, or create a new one if none exists.
  T &getOrDefault() {
    auto *value = uv_key_get(&key_);
    if (value == nullptr) {
      value = new T{};
      uv_key_set(&key_, value);
    }
    return *static_cast<T *>(value);
  }

  /// Get the stored value. If none exists, this will crash.
  T &get() { return *static_cast<T *>(uv_key_get(&key_)); }

  /// Set the stored value.
  void set(T &&value) { getOrDefault() = std::move(value); }

  /// Set the stored value.
  void setCopy(const T &value) { getOrDefault() = value; }

private:
  uv_key_t key_{};
};

/// @}

} // namespace uvco
