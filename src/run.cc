// uvco (c) 2025 Lewin Bormann. See LICENSE for specific terms.
#include <uv.h>

#include "run.h"
#include "scheduler.h"

#include <cstdio>
#include <fmt/core.h>
#include <memory>

namespace uvco {

Loop::Loop(Scheduler::RunMode mode)
    : loop_{std::make_unique<uv_loop_t>()},
      scheduler_{std::make_unique<Scheduler>(mode)} {
  uv_loop_init(loop_.get());
  uv_loop_set_data(loop_.get(), scheduler_.get());
  scheduler_->setUpLoop(loop_.get());
}

Loop::~Loop() {
  // Schedule closing of scheduler, which deletes the prepare handle.
  // Run loop for single turn.
  scheduler_->close();
  runOne();
  if (0 != uv_loop_close(loop_.get())) {
    fmt::print(stderr, "Loop::~Loop(): uv_loop_close() failed; there were "
                       "still resources on the loop.\n");
  }
}

void Loop::runOne() { uv_run(loop_.get(), UV_RUN_ONCE); }
void Loop::run() { uv_run(loop_.get(), UV_RUN_DEFAULT); }
uv_loop_t *Loop::uvloop() const { return loop_.get(); }

Loop::operator uv_loop_t *() const { return loop_.get(); }

} // namespace uvco
