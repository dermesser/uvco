// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "exception.h"
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

template <typename R>
  requires(!std::is_void_v<R>)
using RootFn = std::function<Promise<R>(const Loop &)>;

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

/// Set up event loop, then run main function to set up promises.
/// Finally, clean up once the event loop has finished.
///
/// Equivalent to `runMain([](const Loop &loop) { main(); co_return; })`.
///
/// Note that anything allocated in the `main` function is destroyed before
/// or while any coroutines started by it are running. Make sure to copy
/// all the required arguments into the started coroutines.
///
/// Bad:
/// 
/// ```cpp
/// runMain([](const Loop &loop) {
///   std::string x = "hello";
///   somePromise(&x);
/// {);
/// ```
///
/// Better:
///
/// ```cpp
/// runMain([](const Loop &loop) {
///   std::string x = "hello";
///   // Copy, not reference or pointer.
///   somePromise(x);
/// });
/// ```
///
/// This works because the C++ runtime keeps a copy of the Promise object
/// in the coroutine state. The event loop can schedule the coroutine even
/// after the `main` function has finished. `runMain()` waits for the
/// event loop to finish before returning.
void runMain(const SetupFn &main,
             Scheduler::RunMode mode = Scheduler::RunMode::Deferred);

/// Run a function returning a promise, and return the result once the event
/// loop has finished. Note that for server functions, the event loop typically
/// doesn't finish.
///
/// Essentially the same as `runMain()` but with a return value.
template <typename R>
R runMain(const RootFn<R> &main,
          Scheduler::RunMode mode = Scheduler::RunMode::Deferred) {
  Loop loop{mode};
  if constexpr (std::is_void_v<R>) {
    fmt::print(stderr, "runMain(const RootFn&, ...) used with void return type. You can probably use runMain(const SetupFn&). \n");
  }
  Promise<R> promise = main(loop);
  loop.run();
  if (!promise.ready()) {
    throw UvcoException{
        "Promise not ready but loop done: this is likely a bug in uvco"};
  }
  return promise.core_->slot;
}

/// @}

} // namespace uvco
