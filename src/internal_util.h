
#include <fmt/format.h>
#include <utility>
#include <uv.h>

static constexpr const bool TRACK_LIFETIMES = true;

namespace uvco {

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

} // namespace uvco
