
#pragma once

#include "promise.h"

#include <cstdlib>

namespace uvco {

template <typename T> class BoundedQueue {
public:
  explicit BoundedQueue(size_t capacity) { queue_.reserve(capacity); }

  template <typename U> void push(U &&elem) {
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
  T pop() {
    BOOST_ASSERT(!empty());
    T t = std::move(queue_.at(tail_++));
    tail_ = tail_ % capacity();
    --size_;
    return t;
  }
  size_t size() const { return size_; }
  size_t capacity() const { return queue_.capacity(); }
  bool empty() const { return size() == 0; }
  bool hasSpace() const { return size() < capacity(); }

private:
  std::vector<T> queue_{};
  // Point to next-filled element.
  size_t head_ = 0;
  // Points to next-popped element.
  size_t tail_ = 0;
  size_t size_ = 0;
};

// A bounded-capacity channel for items of type 'T'.
// Can only be written to or read from by one coroutine at a time; more than one
// coroutine waiting is forbidden.
//
// A reader waits while the channel is empty, and is awoken by the first writer.
// A writer waits while the channel is full, and is awoken by the first reader.
template <typename T> class Channel {
public:
  explicit Channel(size_t capacity) : queue_{capacity} {}

  template <typename U> Promise<void> put(U &&value) {
    if (!queue_.hasSpace()) {
      // Block until a reader has popped an item.
      ChannelAwaiter_ awaiter{queue_, write_waiting_};
      BOOST_VERIFY(co_await awaiter);
    }
    queue_.push(std::forward<U>(value));

    // NOTE: this will switch control to the reader until it suspends; keep this
    // in mind.
    //
    // For a filled queue, this will result in a nice lock-step switching back
    // and forth.
    awake_reader();
    co_return;
  }

  Promise<T> get() {
    if (queue_.empty()) {
      ChannelAwaiter_ awaiter{queue_, read_waiting_};
      BOOST_VERIFY(co_await awaiter);
    }
    T item{queue_.pop()};
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

} // namespace uvco
