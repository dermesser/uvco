// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include "close.h"
#include "scheduler.h"

namespace uvco {

bool CloseAwaiter::await_suspend(std::coroutine_handle<> handle) {
  handle_ = handle;
  return true;
}

bool CloseAwaiter::await_ready() const { return closed_; }

void CloseAwaiter::await_resume() {}

void onCloseCallback(uv_handle_t *stream) {
  auto *awaiter = (CloseAwaiter *)stream->data;
  awaiter->closed_ = true;
  if (awaiter->handle_) {
    auto handle = *awaiter->handle_;
    // Doesn't work reliably, because close callbacks are called at the very
    // end. The event loop doesn't turn until a new i/o event occurs.
    // For now, run close coroutines synchronously.
    //
    // LoopData::enqueue(stream, *awaiter->handle_);
    awaiter->handle_.reset();
    handle.resume();
  }
}

} // namespace uvco
