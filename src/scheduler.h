// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include <fmt/format.h>

#include <coroutine>
#include <vector>

/// LoopData is attached to the UV loop as field `data`, and contains the
/// coroutine scheduler. Currently, it works on a fairly simple basis: callbacks
/// can add coroutines for resumption to the scheduler, and the scheduler runs
/// all coroutines once per event loop turn, right after callbacks.
class LoopData {
public:
  LoopData() { resumable_.reserve(16); }

  LoopData(const LoopData &) = delete;
  LoopData(LoopData &&) = default;
  LoopData &operator=(const LoopData &) = delete;
  LoopData &operator=(LoopData &&) = default;
  ~LoopData() {
    // Trick: saves us from having to explicitly define move
    // assignment/constructors.
    if (resumable_.capacity() != 0) {
      uv_check_stop(&check_);
    }
  }

  template <typename UvHandle>
  static LoopData &ofHandle(const UvHandle *uvhandle) {
    return *(LoopData *)(uvhandle->loop->data);
  }

  static void onCheck(uv_check_t *check) {
    fmt::print(stderr, "before runAll\n");
    LoopData &loopData = ofHandle(check);
    loopData.runAll();
    fmt::print(stderr, "after runAll\n");
  }

  void setUpLoop(uv_loop_t *loop) {
    loop->data = this;
    uv_check_init(loop, &check_);
  }

  template <typename UvHandle>
  static void enqueue(const UvHandle *handle,
                      std::coroutine_handle<> corohandle) {
    ofHandle(handle).enqueue(corohandle);
  }
  void enqueue(std::coroutine_handle<> handle) {
    if (resumable_.empty()) {
      uv_check_start(&check_, onCheck);
    }
    resumable_.push_back(handle);
  }
  void runAll() {
    for (auto coro : resumable_) {
      coro.resume();
    }
    resumable_.clear();
    uv_check_stop(&check_);
  }

private:
  std::vector<std::coroutine_handle<>> resumable_ = {};
  uv_check_t check_ = {};
};
