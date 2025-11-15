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
/// it takes about 50 ns to send a small value (e.g. int) on clang-21 and AMD
/// Ryzen 7 PRO 7840U.
///
/// Caveat 1: the channel is obviously not thread safe. Only use within one
/// loop.
///
/// Caveat 2: Channel is independent of a `Loop`. This means that `runMain()`
/// may return despite there being channels in use and awaited on. Ensure that
/// at least one uv operation (socket read/write/listen, timer, etc.) is running
/// to keep the loop alive.
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
    while (!read_waiting_.empty()) {
      std::coroutine_handle<void> handle = read_waiting_.get();
      // Skip cancelled coroutines.
      if (handle != nullptr) {
        Loop::enqueue(handle);
        break;
      }
    }
  }

  void awake_writer() {
    while (!write_waiting_.empty()) {
      std::coroutine_handle<void> handle = write_waiting_.get();
      if (handle != nullptr) {
        Loop::enqueue(handle);
        break;
      }
    }
  }

  struct ChannelAwaiter_ {
    explicit ChannelAwaiter_(BoundedQueue<T> &queue,
                             BoundedQueue<std::coroutine_handle<>> &slot)
        : queue_{queue}, waiters_{slot} {}
    ~ChannelAwaiter_() {
      // Remove us from waiters.
      waiters_.forEach([this](std::coroutine_handle<> &h) {
        // Remove pending coroutine from waiter queue.
        if (h == thisCoro_) {
          h = nullptr;
        }
      });
    }

    bool await_ready() { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
      if (!waiters_.hasSpace()) {
        throw UvcoException(
            UV_EBUSY,
            "too many coroutines waiting for reading/writing a channel");
      }
      thisCoro_ = handle;
      waiters_.put(handle);
      return true;
    }

    bool await_resume() { return !queue_.empty(); }

    // References Channel<T>::queue_
    BoundedQueue<T> &queue_;
    BoundedQueue<std::coroutine_handle<>> &waiters_;
    std::coroutine_handle<> thisCoro_;
  };
};

/// @}

} // namespace uvco
