
#include "close.h"

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
  if (awaiter->handle_)
    awaiter->handle_->resume();
}

} // namespace uvco
