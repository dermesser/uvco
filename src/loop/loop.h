// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "scheduler.h"

#include <coroutine>
#include <memory>
#include <uv.h>

namespace uvco {

/// @addtogroup Loop
/// @{

/// A wrapper around a libuv event loop. Use `uvloop()` to get a reference
/// to the loop, and `run()` to start the event loop.
///
/// Typically this is only used by uvco's internal machinery. User code will
/// pass around a reference to the loop.
///
/// Use `uvco::runMain()` for a top-level interface.
class Loop {
public:
  // Don't use this constructor. Use `runMain()` instead.
  explicit Loop(Scheduler::RunMode mode = Scheduler::RunMode::Deferred);

  Loop(const Loop &) = delete;
  Loop(Loop &&) = delete;
  Loop &operator=(const Loop &) = delete;
  Loop &operator=(Loop &&) = delete;
  ~Loop();

  /// Get a non-owned pointer to the loop.
  [[nodiscard]] uv_loop_t *uvloop() const;

  explicit operator uv_loop_t *() const;

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
