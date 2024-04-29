
#include "exception.h"
#include "internal/internal_utils.h"
#include <fmt/core.h>
#include <string>
#include <string_view>
#include <utility>
#include <uv.h>

namespace uvco {

UvcoException::UvcoException(std::string message) noexcept
    : message{std::move(message)} {}

UvcoException::UvcoException(uv_status status, std::string_view where) noexcept
    : message{fmt::format("UV error {} ({})", uv_err_name(status), where)},
      status{status} {}

const char *UvcoException::what() const noexcept { return message.c_str(); }

UvcoException::operator std::string() const { return message; }

} // namespace uvco
