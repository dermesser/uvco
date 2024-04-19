// uvco (c) 2025 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "loop/loop.h"
#include "loop/scheduler.h"
#include "promise/promise.h"

namespace uvco {

/// @addtogroup Run
/// @{

class Loop;

// Forward declaration only for friend declaration.
template <typename F, typename R>
concept MainFunction = std::is_invocable_r_v<Promise<R>, F, const Loop &>;

template <typename R, MainFunction<R> F>
R runMain(F main, Scheduler::RunMode mode = Scheduler::RunMode::Deferred);

/// Set up event loop, then run main function to set up promises.
/// Finally, clean up once the event loop has finished. An exception
/// thrown within a coroutine is rethrown here.
///
/// `MainFunction` is a function taking a single `const Loop&` argument, and
/// returning a `Promise<R>`. The supplied Loop is necessary to instantiate
/// different types of resources, such as TCP streams or timers.
///
/// Example:
///
/// ```cpp
/// runMain<void>([](const Loop& loop) -> Promise<void> {
///   // Set up resources here.
///   TtyStream stdin = TtyStream::stdin(loop);
///   std::optional<std::string> line = co_await stdin.read();
///   co_await stdin.close();
///   co_return;
/// });
/// ```
///
template <typename R, MainFunction<R> F>
R runMain(F main, Scheduler::RunMode mode) {
  Loop loop{mode};
  Promise<R> promise = main(loop);
  runLoop(loop);
  return promise.unwrap();
}

/// @}

} // namespace uvco
