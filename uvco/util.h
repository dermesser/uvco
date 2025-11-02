// uvco (c) 2025 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <functional>
namespace uvco {

class OnExit {
public:
  explicit OnExit(std::function<void()> &&func) : func_{std::move(func)} {}

  ~OnExit() {
    if (func_) {
      func_();
    }
  }

  // Pin in place
  OnExit(const OnExit &) = delete;
  OnExit(OnExit &&) = delete;
  OnExit &operator=(const OnExit &) = delete;
  OnExit &operator=(OnExit &&) = delete;

private:
  std::function<void()> func_;
};

} // namespace uvco
