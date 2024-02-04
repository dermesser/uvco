
#include <uv.h>

#include "promise/promise.h"
#include "run.h"

#include <functional>

void run_loop(
    const std::function<uvco::Promise<void>(const uvco::Loop &)> &setup) {
  auto innerSetup = [setup](const uvco::Loop &loop) {
    auto promise = setup(loop);
  };
  uvco::runMain(innerSetup);
}

void run_loop(const std::function<uvco::Promise<void>(uv_loop_t *)> &setup) {
  auto innerSetup = [setup](const uvco::Loop &loop) {
    auto promise = setup(loop.uvloop());
  };
  uvco::runMain(innerSetup);
}
