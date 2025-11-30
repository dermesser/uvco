// uvco (c) 2025 Lewin Bormann. See LICENSE for specific terms.

#include "uvco/combinators.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"

#include <boost/program_options/errors.hpp>
#include <exception>
#include <fmt/core.h>

#include <coroutine>
#include <cstddef>
#include <deque>
#include <memory>
#include <unordered_map>
#include <utility>

namespace uvco {

namespace {

struct YieldAwaiter_ {
  [[nodiscard]] static bool await_ready() noexcept { return false; }
  bool await_suspend(std::coroutine_handle<> handle) noexcept {
    Loop::enqueue(handle);
    // Ensure that loop runs existing promises.
    return true;
  }
  void await_resume() const noexcept {}
};

} // namespace

Promise<void> yield() { co_await YieldAwaiter_{}; }

MultiPromise<unsigned> yield(unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    co_yield i;
  }
}

struct WaitPoint::WaitPointAwaiter_ {
  explicit WaitPointAwaiter_(WaitPoint &waitPoint) : waitPoint_{waitPoint} {}

  [[nodiscard]] static bool await_ready() { return false; }

  [[nodiscard]] std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> handle) const {
    waitPoint_.enqueue(handle);
    return Loop::getNext();
  }

  void await_resume() const {}

  WaitPoint &waitPoint_;
};

Promise<void> WaitPoint::wait() { co_await WaitPointAwaiter_{*this}; }

void WaitPoint::releaseOne() {
  if (waiters_.empty()) {
    return;
  }
  Loop::enqueue(waiters_.front());
  waiters_.pop_front();
}

void WaitPoint::releaseAll() {
  while (!waiters_.empty()) {
    releaseOne();
  }
}

void WaitPoint::enqueue(std::coroutine_handle<> handle) {
  waiters_.push_back(handle);
}

namespace {

class TaskSetImpl : public TaskSet {
public:
  TaskSetImpl() = default;
  TaskSetImpl(const TaskSetImpl &) = delete;
  TaskSetImpl(TaskSetImpl &&) = delete;
  TaskSetImpl &operator=(const TaskSetImpl &) = delete;
  TaskSetImpl &operator=(TaskSetImpl &&) = delete;
  ~TaskSetImpl() override = default;

private:
  Id add(Promise<void> task) override {
    const Id taskId = counter_++;
    tasks_.insert({taskId, wrap(taskId, std::move(task))});
    return taskId;
  }

  bool empty() override { return 0 == tasks_.size() - doneTasks_.size(); }

  Promise<void> onEmpty() override {
    if (empty()) {
      co_return;
    }
    co_await onEmpty_.wait();
  }

  void setOnError(ErrorCallback ecb) override {
    errorCallback_ = std::move(ecb);
  }

  Promise<void> wrap(Id taskId, Promise<void> task) {
    // Wait for task to finish; do regular housekeeping; then mark current task
    // as ready for cleanup.
    try {
      co_await task;
    } catch (const std::exception &e) {
      if (errorCallback_) {
        errorCallback_(taskId, std::current_exception());
      } else {
        fmt::print(stderr,
                   "TaskSet task {} failed with exception: {} (setOnError() to "
                   "suppress this message)\n",
                   taskId, e.what());
      }
    } catch (...) {
      if (errorCallback_) {
        errorCallback_(taskId, std::current_exception());
      } else {
        fmt::print(stderr,
                   "TaskSet task {} failed with unknown exception "
                   "(setOnError() to suppress this message)\n",
                   taskId);
      }
    }

    // Already trigger onEmpty if no more tasks are left. Otherwise this leads
    // to a deadlock-like problem, as no more tasks will run to trigger the
    // cleanup, and thus notify onEmpty.
    if (tasks_.size() - doneTasks_.size() - 1 == 0) {
      onEmpty_.releaseAll();
    }

    // Drop finished tasks.
    while (!doneTasks_.empty()) {
      tasks_.erase(doneTasks_.front());
      doneTasks_.pop_front();
    }

    // We cannot erase the task directly here, as the task in tasks_ refers to
    // this very coroutine. Therefore, mark it for cleanup and deal with it
    // later.
    doneTasks_.push_back(taskId);
  }

  Id counter_ = 0;
  std::unordered_map<Id, Promise<void>> tasks_{};
  ErrorCallback errorCallback_;
  // List of finished tasks for delayed clean-up.
  std::deque<Id> doneTasks_{};
  WaitPoint onEmpty_{};
};

} // namespace

std::unique_ptr<TaskSet> TaskSet::create() {
  return std::make_unique<TaskSetImpl>();
}

} // namespace uvco
