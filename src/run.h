// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "scheduler.h"
#include <functional>
#include <memory>
#include <uv.h>

namespace uvco {

/// @addtogroup Run
/// @{

class Loop;

/// SetupFn is a function taking a uv_loop_t*, suitable for
/// initializing promises, and returns to start the event loop.
using SetupFn = std::function<void(const Loop &)>;

/// Set up event loop, then run main function to set up promises.
/// Finally, clean up once the event loop has finished.
void runMain(const SetupFn &main,
             Scheduler::RunMode mode = Scheduler::RunMode::Deferred);

/// A wrapper around a libuv event loop. Use `uvloop()` to get a reference
/// to the loop, and `run()` to start the event loop.
///
/// Use `uvco::runMain()` for a top-level interface.
class Loop {
public:
  Loop(const Loop &) = delete;
  Loop(Loop &&) = default;
  Loop &operator=(const Loop &) = delete;
  Loop &operator=(Loop &&) = default;
  ~Loop();

  /// Get a non-owned pointer to the loop.
  [[nodiscard]] uv_loop_t *uvloop() const;

  explicit operator uv_loop_t *() const;

private:
  friend void runMain(const SetupFn &main, Scheduler::RunMode mode);

  explicit Loop(Scheduler::RunMode mode = Scheduler::RunMode::Deferred);
  /// Run the event loop. This will serve all promises initialized before
  /// calling it.
  void run();
  // Run a single turn of the loop.
  void runOne();

  // Loop and scheduler should be kept at the same
  // place in memory.
  std::unique_ptr<uv_loop_t> loop_;
  std::unique_ptr<Scheduler> scheduler_;
};

/// @}

} // namespace uvco
