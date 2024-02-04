// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include "close.h"
#include "promise/promise.h"

#include <boost/assert.hpp>

#include <coroutine>
#include <vector>

namespace uvco {

/// @addtogroup Scheduler
/// @{

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
  enum class RunMode {
    /// Run an enqueued coroutine immediately, in the stack of the
    /// enqueuing function.
    Immediate = 0,
    /// Run all enqueued coroutines sequentially, after all I/O has been
    /// completed, before the next I/O poll.
    Deferred = 1,
  };

  /// Construct a scheduler.
  ///
  /// If `immediate_resume` is true, resumed coroutines will be run
  /// immediately in the call stack of the resuming function. Otherwise
  /// the coroutine will be resumed upon the next turn of the event loop,
  /// i.e. after all activity has finished and before the next I/O poll,
  /// by call to `runAll()`.
  ///
  /// Call `setUpLoop()` to attach the scheduler to a libuv event loop.
  explicit Scheduler(RunMode mode = RunMode::Deferred) : run_mode_{mode} {
    resumable_.reserve(16);
  }

  Scheduler(const Scheduler &) = delete;
  Scheduler(Scheduler &&) = default;
  Scheduler &operator=(const Scheduler &) = delete;
  Scheduler &operator=(Scheduler &&) = default;
  ~Scheduler();

  /// Obtain a reference to `Scheduler` from any libuv handle.
  template <typename UvHandle>
  static Scheduler &ofHandle(const UvHandle *uvhandle) {
    BOOST_ASSERT(uvhandle != nullptr);
    return *(Scheduler *)uv_loop_get_data(uvhandle->loop);
  }

  /// Set up scheduler with event loop. This is required for all uvco
  /// code to find the scheduler.
  void setUpLoop(uv_loop_t *loop);

  /// Schedule a coroutine for resumption with the scheduler associated with
  /// `handle`.
  template <typename UvHandle>
  static void enqueue(const UvHandle *handle,
                      std::coroutine_handle<> corohandle) {
    ofHandle(handle).enqueue(corohandle);
  }
  /// Schedule a coroutine for resumption.
  void enqueue(std::coroutine_handle<> handle);

  /// Run all scheduled coroutines sequentially.
  void runAll();

  /// close() should be called once the main promise has finished, and the
  /// process is preparing to exit; however, while the event loop is still
  /// running. Example: Once a user has pressed Ctrl-D in a tty application.
  ///
  /// Otherwise, resources may be leaked. (This is usually not super important,
  /// because the event loop is finishing soon after anyway).
  Promise<void> close();

  /// Helper method for `close`,; can be called on any
  static Promise<void> close(const uv_loop_t *loop);

private:
  std::vector<std::coroutine_handle<>> resumable_ = {};
  uv_prepare_t prepare_ = {};
  RunMode run_mode_;

  /// Callback called by the libuv event loop after I/O poll.
  static void onprepare(uv_prepare_t *prepare) {
    Scheduler &loopData = ofHandle(prepare);
    loopData.runAll();
  }
};

} // namespace uvco
