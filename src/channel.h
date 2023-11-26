// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "promise.h"

#include <concepts>
#include <cstdlib>

namespace uvco {

/// @addtogroup Channels Buffered channels for inter-coroutine communication
/// A `Channel` is similar to a Go channel: buffered, and blocking for reading
/// and writing if empty/full respectively.
/// @{

/// A bounded FIFO queue based on a contiguous array.
///
/// Warning: only for internal use. The `push()/pop()` interface is not safe in
/// `Release` mode binaries; `BoundedQueue` is only intended to be used as part
/// of `Channel<T>`.
template <typename T> class BoundedQueue {
public:
  explicit BoundedQueue(size_t capacity) { queue_.reserve(capacity); }

  /// Push an item to the queue.
  template <typename U>
  void put(U &&elem)
    requires std::convertible_to<U, T>
  {
    BOOST_ASSERT(hasSpace());
    if (queue_.size() < capacity()) {
      BOOST_ASSERT(tail_ <= head_);
      queue_.push_back(std::forward<U>(elem));
    } else {
      queue_.at(head_) = std::forward<U>(elem);
    }
    head_ = (head_ + 1) % capacity();
    ++size_;
  }
  /// Pop an item from the queue.
  T get() {
    BOOST_ASSERT(!empty());
    T t = std::move(queue_.at(tail_++));
    tail_ = tail_ % capacity();
    --size_;
    return t;
  }
  /// Current number of contained items.
  size_t size() const { return size_; }
  /// Maximum number of contained items.
  size_t capacity() const { return queue_.capacity(); }
  /// `size() == 0`
  bool empty() const { return size() == 0; }
  /// `size() < capacity()`
  bool hasSpace() const { return size() < capacity(); }

private:
  std::vector<T> queue_{};
  // Point to next-filled element.
  size_t head_ = 0;
  // Points to next-popped element.
  size_t tail_ = 0;
  size_t size_ = 0;
};

/// A bounded-capacity channel for items of type `T`.
/// Can only be written to or read from by one coroutine at a time; more than
/// one coroutine waiting is forbidden.
///
/// A reader waits while the channel is empty, and is awoken by the first
/// writer. A writer waits while the channel is full, and is awoken by the first
/// reader.
///
/// When only using a channel to communicate small objects between coroutines,
/// it takes about 500 ns per send/receive opreation on a slightly older
/// *i5-7300U CPU @ 2.60GHz* CPU. This includes the entire coroutine dance.
template <typename T> class Channel {
public:
  /// Create a channel for up to `capacity` items.
  explicit Channel(size_t capacity) : queue_{capacity} {}

  /// Put an item into the channel.
  ///
  /// Suspends if no space is available in the channel. The suspended coroutine
  /// is resumed by the next reader taking out an item.
  ///
  /// Template method: implements perfect forwarding for both copy and move
  /// insertion.
  ///
  /// NOTE: template argument restriction may not be entirely correct?
  template <typename U>
  Promise<void> put(U &&value)
    requires std::convertible_to<U, T>
  {
    if (!queue_.hasSpace()) {
      // Block until a reader has popped an item.
      ChannelAwaiter_ awaiter{queue_, write_waiting_};
      BOOST_VERIFY(co_await awaiter);
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

private:
  void awake_reader() {
    if (read_waiting_) {
      auto resume = *read_waiting_;
      read_waiting_.reset();
      // Not using LoopData for two reasons: #1 Channel doesn't know about the loop.
      // #2: It is considerably slower at no tangible benefit yet.
      resume.resume();
    }
  }
  void awake_writer() {
    if (write_waiting_) {
      auto resume = *write_waiting_;
      write_waiting_.reset();
      resume.resume();
    }
  }

  BoundedQueue<T> queue_;
  // TODO: a multi-reader/writer queue is easily achieved by converting the
  // optionals into queues. This may be interesting in future.
  std::optional<std::coroutine_handle<>> read_waiting_;
  std::optional<std::coroutine_handle<>> write_waiting_;

  struct ChannelAwaiter_ {
    explicit ChannelAwaiter_(BoundedQueue<T> &queue,
                             std::optional<std::coroutine_handle<>> &slot)
        : queue_{queue}, slot_{slot} {}
    bool await_ready() { return false; }
    bool await_suspend(std::coroutine_handle<> handle) {
      // BOOST_ASSERT(!slot_);
      if (slot_) {
        throw UvcoException(
            UV_EBUSY,
            "only one coroutine can wait for reading/writing a channel");
      }
      slot_ = handle;
      return true;
    }
    bool await_resume() { return !queue_.empty(); }

    BoundedQueue<T> &queue_;
    std::optional<std::coroutine_handle<>> &slot_;
  };
};

/// @}

} // namespace uvco
