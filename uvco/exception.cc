
#include "uvco/exception.h"
#include "uvco/internal/internal_utils.h"

#include <array>
#include <fmt/core.h>
#include <uv.h>

#include <string>
#include <string_view>
#include <utility>

namespace uvco {

UvcoException::UvcoException(std::string message) noexcept
    : message{std::move(message)} {}

UvcoException::UvcoException(uv_status status, std::string_view where) noexcept
    : status{status} {
  std::array<char, 128> errName;
  message =
      fmt::format("UV error {} ({})",
                  uv_err_name_r(status, errName.data(), errName.size()), where);
}

const char *UvcoException::what() const noexcept { return message.c_str(); }

} // namespace uvco
