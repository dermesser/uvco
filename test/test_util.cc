
#include <uv.h>

#include "uvco/loop/scheduler.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"

#include <functional>

void run_loop(
    const std::function<uvco::Promise<void>(const uvco::Loop &)> &setup) {
  uvco::runMain<void>(setup);
}
