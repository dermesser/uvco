// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <boost/assert.hpp>
#include <uv.h>

#include <coroutine>
#include <deque>

namespace uvco {

/// @addtogroup Scheduler
/// @{

/// If set to true, log scheduler operations to stdout.
static constexpr bool logSchedulerOperations = false;

/// The Scheduler is attached to the UV loop as field `data`, and contains the
/// coroutine scheduler. Currently, it works on a fairly simple basis: callbacks
/// can add coroutines for resumption to the scheduler, and the scheduler runs
/// all coroutines once per event loop turn, right after callbacks.
///
/// This is in contrast to the "conventional" model, on which most asynchronous
/// code in uvco is built: Almost all resumptions are triggered by libuv
/// callbacks during I/O polling; the resumed coroutines work on the stack of
/// the callback. for awaiting promises, the waiting coroutine is run directly
/// on the stack of the coroutine resolving the promise in question.
///
/// This has the lowest latency but has some downsides; mainly, execution of
/// coroutines is very interleaved. There may be unexpected internal states
/// while executing such an interleaved system of coroutines.
///
/// In contrast, this scheduler works through all waiting (but ready) coroutines
/// sequentially. Each resumed coroutine returns when it is either finished or
/// has reached its next suspension point. This is easier to understand, but
/// incurs the cost of first enqueuing a coroutine and only executing it after
/// all I/O has been polled.
///
/// This is currently *only* used for UDP sockets. It is intended to be 100%
/// compatible with the conventional model. The benefit of using this scheduler
/// is currently unclear (theoretically, it simplifies the execution stack and
/// makes bugs less likely, or easier to find due to non-interleaved execution
/// of coroutines), thus it is not introduced everywhere yet.
class Scheduler {
public:
  Scheduler();
  Scheduler(const Scheduler &) = delete;
  Scheduler(Scheduler &&) = delete;
  Scheduler &operator=(const Scheduler &) = delete;
  Scheduler &operator=(Scheduler &&) = delete;
  ~Scheduler();

  /// Schedule a coroutine for resumption.
  void enqueue(std::coroutine_handle<> handle);

  void cancel(std::coroutine_handle<> handle);

  /// Run all scheduled coroutines sequentially.
  void runAll();

  /// Returns a handle to the next runnable suspended coroutine. This is mainly
  /// used by Loop::getNext() which in turns is used to implement symmetric
  /// hand-off, i.e. decentralized scheduling. By using this, all suspending
  /// awaiters can hand off control directly to the next coroutine instead of
  /// going through the scheduler first. This behavior can be disabled by
  /// setting the `useSymmetricHandoff` compile-time variable to false in
  /// scheduler.cc.
  std::coroutine_handle<> getNext();

  /// close() should be called once the main promise has finished, and the
  /// process is preparing to exit; however, while the event loop is still
  /// running. Example: Once a user has pressed Ctrl-D in a tty application.
  ///
  /// Otherwise, resources may be leaked. (This is usually not super important,
  /// because the event loop is finishing soon after anyway).
  void close();

  [[nodiscard]] bool empty() const { return resumable_.empty(); }

private:
  std::coroutine_handle<> getNextInner();

  std::deque<std::coroutine_handle<>> resumable_;
};

} // namespace uvco
