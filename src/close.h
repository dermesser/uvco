// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "promise.h"

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

template<typename T, typename C, typename H = T>
Promise<void> closeHandle(T* handle, C closer) {
  CloseAwaiter awaiter{};
  handle->data = &awaiter;
  closer((H*)handle, onCloseCallback);
  co_await awaiter;
}

template<typename T>
Promise<void> closeHandle(T* handle) {
  co_await closeHandle((uv_handle_t*)handle, uv_close);
}

} // namespace uvco
