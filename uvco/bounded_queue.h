// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <concepts>
#include <uv.h>

#include "uvco/exception.h"

#include <boost/assert.hpp>
#include <utility>
#include <vector>

namespace uvco {

/// @addtogroup Channel

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
    if (!hasSpace()) {
      throw UvcoException(UV_EBUSY, "queue is full");
    }
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
    if (empty()) [[unlikely]] {
      throw UvcoException(UV_EAGAIN, "queue is empty");
    }
    T element = std::move(queue_.at(tail_++));
    tail_ = tail_ % capacity();
    --size_;
    return element;
  }
  /// Current number of contained items.
  [[nodiscard]] unsigned size() const { return size_; }
  /// Maximum number of contained items.
  [[nodiscard]] unsigned capacity() const { return queue_.capacity(); }
  /// `size() == 0`
  [[nodiscard]] bool empty() const { return size() == 0; }
  /// `size() < capacity()`
  [[nodiscard]] bool hasSpace() const { return size() < capacity(); }

private:
  std::vector<T> queue_{};
  // Point to next-filled element.
  unsigned head_ = 0;
  // Points to next-popped element.
  unsigned tail_ = 0;
  unsigned size_ = 0;
};

/// @}

} // namespace uvco
