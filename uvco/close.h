// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <cstdio>
#include <uv.h>

#include "uvco/promise/promise.h"

#include <boost/assert.hpp>
#include <coroutine>
#include <optional>

namespace uvco {

struct CloseAwaiter {
  explicit CloseAwaiter(uv_handle_t *handle) : uvHandle_{handle} {}
  ~CloseAwaiter() { setData(uvHandle_, (void *)nullptr); }

  [[nodiscard]] bool await_ready() const;
  bool await_suspend(std::coroutine_handle<> handle);
  void await_resume();

  std::optional<std::coroutine_handle<>> handle_;
  uv_handle_t *uvHandle_;
  bool closed_ = false;
};

void onCloseCallback(uv_handle_t *handle);

// closeHandle() takes care of safely closing a handle. Canonically you should
// await the returned promise to be sure that the handle is closed. However, if
// the promise is dropped and thus the coroutine cancelled, the libuv close
// operation will still be carried out safely in the background.

template <typename Handle, typename CloserArg>
Promise<void> closeHandle(Handle *handle,
                          void (*closer)(CloserArg *,
                                         void (*)(uv_handle_t *))) {
  BOOST_ASSERT(handle != nullptr);
  CloseAwaiter awaiter{(uv_handle_t *)handle};
  setData(handle, &awaiter);
  closer((CloserArg *)handle, onCloseCallback);
  co_await awaiter;
  setData(handle, (void *)nullptr);
  BOOST_ASSERT(awaiter.closed_);
}

template <typename Handle> Promise<void> closeHandle(Handle *handle) {
  return closeHandle<Handle, uv_handle_t>(handle, uv_close);
}

} // namespace uvco
