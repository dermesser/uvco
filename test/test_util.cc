
#include <uv.h>

#include "promise.h"

#include <functional>

void run_loop(const std::function<uvco::Promise<void>(uv_loop_t *)>& setup) {
  uv_loop_t loop;
  uv_loop_init(&loop);

  uvco::Promise<void> promise = setup(&loop);

  uv_run(&loop, UV_RUN_DEFAULT);

  //BOOST_ASSERT(promise.ready());

  uv_loop_close(&loop);
}
