// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <cstdio>
#include <uv.h>

#include "promise/promise.h"

#include <boost/assert.hpp>
#include <coroutine>
#include <optional>

namespace uvco {

struct CloseAwaiter {
  [[nodiscard]] bool await_ready() const;
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
  handle->data = nullptr;
  BOOST_ASSERT(awaiter.closed_);
}

template <typename T> Promise<void> closeHandle(T *handle) {
  return closeHandle((uv_handle_t *)handle, uv_close);
}

} // namespace uvco
