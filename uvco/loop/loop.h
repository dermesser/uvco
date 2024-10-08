// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include "uvco/loop/scheduler.h"

#include <coroutine>
#include <memory>

namespace uvco {

/// @addtogroup Loop
/// @{

/// Not used by user code! Use `runMain()` for the top-level interface.
///
/// A wrapper around a libuv event loop. `uvloop()` returns a reference
/// to the loop, and `run()` starts the event loop. `enqueue()` schedules a
/// coroutine to run on the default loop at a later time, enabling any part of
/// uvco to easily schedule work on the current loop.
///
/// Typically this is only used by uvco's internal machinery. User code will
/// pass around a reference to the loop.
class Loop {
public:
  // Don't use this constructor. Use `runMain()` instead.
  Loop();
  Loop(const Loop &) = delete;
  Loop(Loop &&) = delete;
  Loop &operator=(const Loop &) = delete;
  Loop &operator=(Loop &&) = delete;
  ~Loop();

  /// Get a non-owned pointer to the loop.
  [[nodiscard]] uv_loop_t *uvloop() const;

  // Retrieve the currently active global scheduler associated with the default
  // loop.
  static void enqueue(std::coroutine_handle<> handle);

private:
  // The default loop is the only loop that can be created. It is set/unset by
  // the constructor/destructor.
  static Loop *defaultLoop;
  static Scheduler &currentScheduler();

  friend void runLoop(Loop &);
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
