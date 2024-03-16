#pragma once

#include <uv.h>

#include "internal/internal_utils.h"

#include <fmt/format.h>

#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace uvco {

/// @addtogroup Exception
/// @{
/// The `UvcoException` class is used for all exceptions thrown by `uvco`.

/// An `UvcoException` wraps a numeric `libuv` status code as well as a message.
/// The status code is automatically converted to a string, which can be
/// obtained using `what()`.
struct UvcoException : public std::exception {
  explicit UvcoException(std::string message) noexcept
      : message{std::move(message)} {}
  UvcoException(uv_status status, std::string_view where) noexcept
      : message{fmt::format("UV error {} ({})", uv_err_name(status), where)},
        status{status} {}
  UvcoException &operator=(const UvcoException &) noexcept = default;
  UvcoException &operator=(UvcoException &&) noexcept = default;
  UvcoException(const UvcoException &) noexcept = default;
  UvcoException(UvcoException &&) noexcept = default;
  ~UvcoException() noexcept override = default;

  /// Provide information about the error.
  [[nodiscard]] const char *what() const noexcept override {
    return message.c_str();
  }

  /// Like `what()`.
  explicit operator std::string() const { return message; }

  /// The error message.
  std::string message;
  /// The status. An UvcoException may not be caused by a libuv error, in which
  /// case status is `nullopt`.
  std::optional<uv_status> status;
};

/// @}

} // namespace uvco
