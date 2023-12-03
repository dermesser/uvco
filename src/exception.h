#pragma once

#include <uv.h>

#include <fmt/format.h>

#include <exception>
#include <string>
#include <string_view>

namespace uvco {

/// Result of a libuv operation.
using uv_status = int;

struct UvcoException : public std::exception {
  explicit UvcoException(std::string message) : message_{std::move(message)} {}
  explicit UvcoException(uv_status status, std::string_view where)
      : message_{fmt::format("UV error {} ({})", uv_err_name(status), where)} {}
  [[nodiscard]] const char *what() const noexcept override {
    return message_.c_str();
  }
  explicit operator std::string() const { return message_; }
  std::string message_;
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

} // namespace uvco
