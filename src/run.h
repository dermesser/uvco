// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "promise/promise.h"
#include "scheduler.h"

#include <memory>
#include <type_traits>
#include <uv.h>

namespace uvco {

/// @addtogroup Run
/// @{

class Loop;

template <typename F, typename R>
concept MainFunction = std::is_invocable_r_v<Promise<R>, F, const Loop &>;

template <typename R, MainFunction<R> F>
R runMain(F main, Scheduler::RunMode mode = Scheduler::RunMode::Deferred);

/// A wrapper around a libuv event loop. Use `uvloop()` to get a reference
/// to the loop, and `run()` to start the event loop.
///
/// Typically this is only used by uvco's internal machinery. User code will
/// pass around a reference to the loop.
///
/// Use `uvco::runMain()` for a top-level interface.
class Loop {
public:
  Loop(const Loop &) = delete;
  Loop(Loop &&) = delete;
  Loop &operator=(const Loop &) = delete;
  Loop &operator=(Loop &&) = delete;
  ~Loop();

  /// Get a non-owned pointer to the loop.
  [[nodiscard]] uv_loop_t *uvloop() const;

  explicit operator uv_loop_t *() const;

private:
  template <typename R, MainFunction<R> F>
  friend R runMain(F main, Scheduler::RunMode mode);

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

/// Set up event loop, then run main function to set up promises.
/// Finally, clean up once the event loop has finished. An exception
/// thrown within a coroutine is rethrown here.
template <typename R, MainFunction<R> F>
R runMain(F main, Scheduler::RunMode mode) {
  Loop loop{mode};
  Promise<R> promise = main(loop);
  loop.run();
  return promise.unwrap();
}

/// @}

} // namespace uvco
