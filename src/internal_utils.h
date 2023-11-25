// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <fmt/format.h>
#include <utility>
#include <uv.h>

namespace uvco {

/// `RefCounted<T>` is an intrusive refcounting approach, which shaves up to 50%
/// performance off of low-overhead high frequency promise code (such as
/// buffered channel ping-pong scenarios). This is where `shared_ptr` performs badly;
/// in turn, manual refcounting is required by objects owning a refcounted object.
template <typename T> class RefCounted {
public:
  RefCounted() = default;
  // Assignment doesn't change count.
  RefCounted(const RefCounted &other) = default;
  RefCounted &operator=(const RefCounted &other) = default;
  RefCounted(RefCounted &&other) noexcept {}
  RefCounted &operator=(RefCounted &&other) noexcept {}
  virtual ~RefCounted() = default;

  T *addRef() {
    ++count_;
    return static_cast<T *>(this);
  }
  void delRef() {
    --count_;
    if (count_ == 0) {
      delete static_cast<T *>(this);
    }
  }

private:
  size_t count_ = 1;
};

extern const bool TRACK_LIFETIMES;

template <typename T> class LifetimeTracker {
public:
  explicit LifetimeTracker(std::string id = "") : id_{std::move(id)} {
    if (TRACK_LIFETIMES)
      fmt::print("ctor {}()#{}\n", typeid(T).name(), id_);
  }
  const LifetimeTracker<T> operator=(const LifetimeTracker<T> &other) {
    if (TRACK_LIFETIMES)
      fmt::print("operator={}({})#{}\n", typeid(T).name(), other.id_, id_);
    id_ = fmt::format("{}/copy", other.id_);
  }
  LifetimeTracker(const LifetimeTracker<T> &other)
      : id_{fmt::format("{}/copy", other.id_)} {
    if (TRACK_LIFETIMES)
      fmt::print("operator={}({})#{}\n", typeid(T).name(), other.id_, id_);
  }
  ~LifetimeTracker() {
    if (TRACK_LIFETIMES)
      fmt::print("dtor ~{}()\n", typeid(T).name());
  }

protected:
  std::string id_;
};

struct UvcoException : public std::exception {
  explicit UvcoException(std::string message) : message_{std::move(message)} {}
  explicit UvcoException(int status, std::string_view where)
      : message_{fmt::format("UV error {} ({})", uv_err_name(status), where)} {}
  [[nodiscard]] const char *what() const noexcept override {
    return message_.c_str();
  }
  explicit operator std::string() const { return message_; }
  const std::string message_;
};

void log(uv_loop_t *loop, std::string_view message);

void allocator(uv_handle_t * /*unused*/, size_t sugg, uv_buf_t *buf);

void freeUvBuf(const uv_buf_t *buf);

struct UvHandleDeleter {
  static void del(uv_handle_t *handle);
  template <typename Handle> void operator()(Handle *handle) {
    del((uv_handle_t *)handle);
  }
};

class FlagGuard {
public:
  FlagGuard(const FlagGuard &) = delete;
  FlagGuard(FlagGuard &&) = delete;
  FlagGuard &operator=(const FlagGuard &) = delete;
  FlagGuard &operator=(FlagGuard &&) = delete;

  explicit FlagGuard(bool &flag);
  ~FlagGuard() { flag_ = false; }

private:
  bool &flag_;
};

} // namespace uvco
