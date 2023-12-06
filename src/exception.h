#pragma once

#include <uv.h>

#include "internal_utils.h"

#include <fmt/format.h>

#include <exception>
#include <string>
#include <string_view>

namespace uvco {

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

} // namespace uvco
