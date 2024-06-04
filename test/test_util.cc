
#include <uv.h>

#include "uvco/loop/scheduler.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"

#include <functional>

namespace {

constexpr uvco::Scheduler::RunMode runMode = uvco::Scheduler::RunMode::Deferred;

} // namespace

void run_loop(
    const std::function<uvco::Promise<void>(const uvco::Loop &)> &setup) {
  uvco::runMain<void>(setup, runMode);
}
