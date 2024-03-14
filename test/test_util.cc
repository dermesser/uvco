
#include <uv.h>

#include "promise/promise.h"
#include "run.h"

#include <functional>

namespace {

constexpr uvco::Scheduler::RunMode runMode =
    uvco::Scheduler::RunMode::Immediate;

} // namespace

void run_loop(
    const std::function<uvco::Promise<void>(const uvco::Loop &)> &setup) {
  auto innerSetup = [setup](const uvco::Loop &loop) -> uvco::Promise<void> {
    co_await setup(loop);
  };
  uvco::runMain(innerSetup, runMode);
}

void run_loop(const std::function<uvco::Promise<void>(uv_loop_t *)> &setup) {
  auto innerSetup = [setup](const uvco::Loop &loop) { setup(loop.uvloop()); };
  uvco::runMain(innerSetup, runMode);
}
