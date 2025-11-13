// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <concepts>
#include <functional>
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
    requires std::convertible_to<U, T>;
  /// Pop an item from the queue.
  T get();
  /// Current number of contained items.
  [[nodiscard]] unsigned size() const { return size_; }
  /// Maximum number of contained items.
  [[nodiscard]] unsigned capacity() const { return queue_.capacity(); }
  /// `size() == 0`
  [[nodiscard]] bool empty() const { return size() == 0; }
  /// `size() < capacity()`
  [[nodiscard]] bool hasSpace() const { return size() < capacity(); }

  void forEach(std::function<void(T &)> function);

private:
  std::vector<T> queue_{};
  // Point to next-filled element.
  unsigned head_ = 0;
  // Points to next-popped element.
  unsigned tail_ = 0;
  unsigned size_ = 0;
};

template <typename T>
template <typename U>
inline void BoundedQueue<T>::put(U &&elem)
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

template <typename T> inline T BoundedQueue<T>::get() {
  if (empty()) [[unlikely]] {
    throw UvcoException(UV_EAGAIN, "queue is empty");
  }
  T element = std::move(queue_.at(tail_++));
  tail_ = tail_ % capacity();
  --size_;
  return element;
}

template <typename T>
void BoundedQueue<T>::forEach(std::function<void(T &)> function) {
  unsigned idx = tail_;
  for (unsigned i = 0; i < size_; ++i) {
    function(queue_.at(idx));
    idx = (idx + 1) % capacity();
  }
}

/// @}

} // namespace uvco
