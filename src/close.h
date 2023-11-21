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

template <typename T, typename C>
Promise<void> closeHandle(T *handle, C closer) {
  BOOST_ASSERT(handle != nullptr);
  CloseAwaiter awaiter{};
  handle->data = &awaiter;
  closer(handle, onCloseCallback);
  co_await awaiter;
  BOOST_ASSERT(awaiter.closed_);
}

template <typename T> Promise<void> closeHandle(T *handle) {
  return closeHandle((uv_handle_t *)handle, uv_close);
}

} // namespace uvco
