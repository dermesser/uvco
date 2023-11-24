
#pragma once

#include "promise.h"

#include <cstdlib>

namespace uvco {

template <typename T> class BoundedQueue {
public:
  explicit BoundedQueue(size_t capacity) { queue_.reserve(capacity); }

  template <typename U> void push(U &&elem) {
    BOOST_ASSERT(size() < queue_.capacity());
    if (queue_.size() < queue_.capacity()) {
      BOOST_ASSERT(tail_ <= head_);
      queue_.push_back(std::forward<U>(elem));
    } else {
      queue_.at(head_) = std::forward<U>(elem);
    }
    head_ = (head_ + 1) % queue_.capacity();
    ++size_;
  }
  T pop() {
    BOOST_ASSERT(size() > 0);
    T t = std::move(queue_.at(tail_++));
    tail_ = tail_ % queue_.capacity();
    --size_;
    return t;
  }
  size_t size() const {
    return size_;
  }
  bool empty() const { return size() == 0; }

private:
  std::vector<T> queue_{};
  // Point to next-filled element.
  size_t head_ = 0;
  // Points to next-popped element.
  size_t tail_ = 0;
  size_t size_ = 0;
};

// A bounded-capacity channel for items of type 'T'.
template <typename T> class Channel {
public:
  explicit Channel(size_t capacity) {}

private:
};

} // namespace uvco
