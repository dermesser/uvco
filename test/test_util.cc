
#include <uv.h>

#include "promise/promise.h"
#include "scheduler.h"

#include <functional>

void run_loop(const std::function<uvco::Promise<void>(uv_loop_t *)> &setup) {
  // You can use `Immediate` to run coroutines directly after enqueuing,
  // instead of later on the event loop.
  uvco::Scheduler loopData{uvco::Scheduler::RunMode::Deferred};

  uv_loop_t loop;
  uv_loop_init(&loop);
  loopData.setUpLoop(&loop);

  auto fixture = [&loop, &setup]() -> uvco::Promise<void> {
    co_await setup(&loop);
    co_await uvco::Scheduler::close(&loop);
    co_return;
  };
  uvco::Promise<void> promise = fixture();

  uv_run(&loop, UV_RUN_DEFAULT);

  BOOST_ASSERT(promise.ready());

  uv_loop_close(&loop);
}
