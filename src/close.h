// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <coroutine>
#include <optional>

#include <uv.h>

namespace uvco {

struct CloseAwaiter {
  bool await_ready() const;
  bool await_suspend(std::coroutine_handle<> handle);
  void await_resume();

  std::optional<std::coroutine_handle<>> handle_;
  bool closed_ = false;
};

void onCloseCallback(uv_handle_t *stream);

} // namespace uvco
