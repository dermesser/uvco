// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include "uvco/bounded_queue.h"
#include "uvco/exception.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"

#include <boost/assert.hpp>
#include <coroutine>

namespace uvco {

/// @addtogroup Channels
/// @{

/// A `Channel` is similar to a Go channel: buffered, and blocking for reading
/// and writing if empty/full respectively.
///
/// A bounded-capacity channel for items of type `T`. A channel can be waited
/// on by at most `max_waiters` coroutines. If more coroutines want to wait,
/// an exception is thrown.
///
/// A reader waits while the channel is empty, and is awoken by the first
/// writer. A writer waits while the channel is full, and is awoken by the first
/// reader.
///
/// When only using a channel to communicate small objects between coroutines,
/// it takes about 1 µs per send/receive operation on a slightly older
/// *i5-7300U CPU @ 2.60GHz* CPU (clang 17) using the `RunMode::Deferred` event
/// loop mode. This includes the entire coroutine dance of suspending/resuming
/// between the reader and writer. (`RunMode::Immediate` is ~25% faster)
///
/// Caveat 1: the channel is obviously not thread safe. Only use within one
/// loop. Caveat 2: As you can notice, the Channel is independent of a `Loop`.
/// This means that a `runMain()` may return despite there being channels in use
/// and awaited on. Ensure that at least one uv operation (socket
/// read/write/listen, timer, etc.) is running to keep the loop alive.
template <typename T> class Channel {
public:
  /// Create a channel for up to `capacity` items.
  explicit Channel(unsigned capacity, unsigned max_waiters = 16)
      : queue_{capacity}, read_waiting_{max_waiters},
        write_waiting_{max_waiters} {}

  /// Put an item into the channel.
  ///
  /// Suspends if no space is available in the channel. The suspended coroutine
  /// is resumed by the next reader taking out an item.
  ///
  /// Template method: implements perfect forwarding for both copy and move
  /// insertion.
  ///
  /// The argument is only accepted by value, as it's safer to do so in a
  /// coroutine.
  template <typename U> Promise<void> put(U value) {
    if (!queue_.hasSpace()) {
      // Block until a reader has popped an item.
      ChannelAwaiter_ awaiter{queue_, write_waiting_};
      // Return value indicates if queue is filled with >= 1 item (true) or
      // empty (false).
      co_await awaiter;
      BOOST_VERIFY(queue_.hasSpace());
    }
    queue_.put(std::forward<U>(value));

    // NOTE: this will switch control to the reader until it suspends; keep this
    // in mind.
    //
    // For a filled queue, this will result in a nice lock-step switching back
    // and forth.
    awake_reader();
    co_return;
  }

  /// Take an item from the channel.
  ///
  /// Suspends if no items are available. The suspended coroutine is resumed by
  /// the next writer adding an item.
  Promise<T> get() {
    if (queue_.empty()) {
      ChannelAwaiter_ awaiter{queue_, read_waiting_};
      BOOST_VERIFY(co_await awaiter);
    }
    T item{queue_.get()};
    // NOTE: this will switch control to the writer until it suspends; keep this
    // in mind.
    awake_writer();
    co_return item;
  }

  /// Continuously read items from channel by repeatedly `co_await`ing the
  /// returned MultiPromise. (getAll() is a generator)
  ///
  /// Remember to call `MultiPromise<T>::cancel()` when you are done with the
  /// channel, although this should happen automatically when the last
  /// MultiPromise object is dropped.
  MultiPromise<T> getAll() {
    while (true) {
      if (queue_.empty()) {
        ChannelAwaiter_ awaiter{queue_, read_waiting_};
        BOOST_VERIFY(co_await awaiter);
      }
      T item = queue_.get();
      awake_writer();
      // Suspends until consumer asks for next item.
      co_yield item;
    }
  }

private:
  BoundedQueue<T> queue_;
  // TODO: a multi-reader/writer queue is easily achieved by converting the
  // optionals into queues. This may be interesting in future.
  BoundedQueue<std::coroutine_handle<>> read_waiting_;
  BoundedQueue<std::coroutine_handle<>> write_waiting_;

  void awake_reader() {
    if (!read_waiting_.empty()) {
      auto handle = read_waiting_.get();
      // Slower than direct resume but interacts more nicely with other
      // coroutines.
      Loop::enqueue(handle);
    }
  }
  void awake_writer() {
    if (!write_waiting_.empty()) {
      auto handle = write_waiting_.get();
      Loop::enqueue(handle);
    }
  }

  struct ChannelAwaiter_ {
    explicit ChannelAwaiter_(BoundedQueue<T> &queue,
                             BoundedQueue<std::coroutine_handle<>> &slot)
        : queue_{queue}, waiters_{slot} {}

    bool await_ready() { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
      if (!waiters_.hasSpace()) {
        throw UvcoException(
            UV_EBUSY,
            "too many coroutines waiting for reading/writing a channel");
      }
      waiters_.put(handle);
      return true;
    }

    bool await_resume() { return !queue_.empty(); }

    // References Channel<T>::queue_
    BoundedQueue<T> &queue_;
    BoundedQueue<std::coroutine_handle<>> &waiters_;
  };
};

/// @}

} // namespace uvco
