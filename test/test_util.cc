
#include <uv.h>

#include "promise/promise.h"
#include "run.h"

#include <functional>

namespace {

constexpr uvco::Scheduler::RunMode runMode = uvco::Scheduler::RunMode::Deferred;

} // namespace

void run_loop(
    const std::function<uvco::Promise<void>(const uvco::Loop &)> &setup) {
  uvco::runMain<void>(setup, runMode);
}
