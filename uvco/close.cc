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

void onCloseCallback(uv_handle_t *handle) {
  auto *awaiter = getDataOrNull<CloseAwaiter>(handle);
  if (awaiter == nullptr) {
    // no CloseAwaiter associated with handle. This means that nobody is
    // awaiting the close, i.e. `co_await X.close()` was not used.
    UvHandleDeleter::del(handle);
    return;
  }
  awaiter->closed_ = true;
  if (awaiter->handle_) {
    auto handle = *awaiter->handle_;
    awaiter->handle_.reset();
    Loop::enqueue(handle);
  }
}

} // namespace uvco
