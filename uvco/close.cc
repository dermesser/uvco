// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <uv.h>

#include "uvco/close.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/loop/loop.h"
#include "uvco/run.h"

#include <coroutine>

namespace uvco {

bool CloseAwaiter::await_suspend(std::coroutine_handle<> handle) {
  handle_ = handle;
  return true;
}

bool CloseAwaiter::await_ready() const { return closed_; }

void CloseAwaiter::await_resume() {}

void onCloseCallback(uv_handle_t *stream) {
  auto *awaiter = getData<CloseAwaiter>(stream);
  awaiter->closed_ = true;
  if (awaiter->handle_) {
    auto handle = *awaiter->handle_;
    awaiter->handle_.reset();
    Loop::enqueue(handle);
  }
}

} // namespace uvco
