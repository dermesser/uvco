
#include <uv.h>

#include "promise/promise.h"
#include "run.h"

#include <functional>

void run_loop(const std::function<uvco::Promise<void>(uv_loop_t *)> &setup) {
  auto innerSetup = [setup](uv_loop_t *loop) {
    auto promise = setup(loop);
  };
  uvco::runMain(setup);
}
