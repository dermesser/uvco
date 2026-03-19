// Copyright (C) 2026 kimapr.  See LICENSE for specific terms.

#pragma once

#include <coroutine>
#include <cstddef>

namespace uvco {

class CoroutineHandle;

class PromiseRefcounter_ {
  size_t refs = 1;
  size_t weaks = 0;
  PromiseRefcounter_ *parent = nullptr; // weakref
  struct { void (*fun)(void*) = nullptr; void *data = nullptr; } deleter;

  // return true if delete
  bool checkRefs() {
    if (deleter.data && !refs) {
      auto data = deleter.data;
      deleter.data = nullptr;
      deleter.fun(data);
      if (parent) {
        auto p = parent;
        parent = nullptr;
        if (p->releaseWeak())
          delete p;
      }
    }
    if (refs || weaks) return false;
    return true;
  }
public:
  PromiseRefcounter_(decltype(deleter.fun) fun, decltype(deleter.data) data) {
    deleter.fun = fun;
    deleter.data = data;
  }

  size_t uses() { return refs; }
  size_t weak_uses() { return weaks; }

  void setParent(PromiseRefcounter_ *np) {
    if (parent) parent->releaseWeak();
    parent = np;
    if (parent)
      parent->acquireWeak();
  }

  inline void setParent(const CoroutineHandle&);

  bool release() {
    BOOST_ASSERT(refs);
    refs--; return checkRefs(); }
  bool releaseWeak() {
    BOOST_ASSERT(weaks);
    weaks--; return checkRefs(); }
  void acquire() {
    refs++;
  }
  void acquireWeak() {
    weaks++;
  }

  static void acquireRecursive(PromiseRefcounter_ *ref) {
    while (ref) {
      ref->refs++;
      if (ref->parent && !ref->parent->refs) {
        if (ref->parent->releaseWeak())
          delete ref->parent;
        ref->parent = nullptr;
      }
      ref = ref->parent;
    }
  }

  static void releaseRecursive(PromiseRefcounter_ *ref) {
    while (ref) {
      ref->refs--;
      auto p = ref->parent;
      if (p) p->weaks++;
      if (ref->checkRefs())
        delete ref;
      if (p) p->weaks--;
      ref = p;
    }
  }
};

class ObtainCoroutineHandleAwaiter;

/// Represents a type-erased coroutine handle with refcounting information
/// preserved.
class CoroutineHandle {
  std::coroutine_handle<> handle;
  PromiseRefcounter_ *ref = nullptr;
  friend class CancellationBlock;
  friend class PromiseRefcounter_;

public:
  CoroutineHandle() = default;

  /// Construct a `CoroutineHandle` from a typed coroutine handle (typically
  /// obtained from a templated `await_suspend()`).
  template<class T>
  CoroutineHandle(std::coroutine_handle<T> handle) : handle(handle), ref(handle.promise().core_.refcount_.inner) { ref->acquireWeak(); }
  CoroutineHandle(std::coroutine_handle<> handle, PromiseRefcounter_ *ref) : handle(handle), ref(ref) { ref->acquireWeak(); }
  CoroutineHandle(std::nullptr_t) {}

  ~CoroutineHandle() { if (ref) if(ref->releaseWeak()) delete ref; }

  CoroutineHandle(const CoroutineHandle &rhs) : handle(rhs.handle), ref(rhs.ref) { if(ref) ref->acquireWeak(); }
  CoroutineHandle(CoroutineHandle &&rhs) : handle(rhs.handle), ref(rhs.ref) { rhs.handle = {}; rhs.ref = {}; }
  CoroutineHandle &operator=(const CoroutineHandle &rhs) {
    if (*this == rhs) return *this;
    if (ref) if (ref->releaseWeak()) delete ref;
    handle = rhs.handle; ref = rhs.ref;
    if (ref) ref->acquireWeak();
    return *this;
  }
  CoroutineHandle &operator=(CoroutineHandle &&rhs) {
    if (*this == rhs) return *this;
    if (ref) if (ref->releaseWeak()) delete ref;
    handle = rhs.handle; ref = rhs.ref;
    rhs.handle = {}; rhs.ref = {};
    return *this;
  }

  operator std::coroutine_handle<> () const {
    if (!*this) return std::noop_coroutine();
    return handle;
  }

  bool operator==(std::nullptr_t) const {
    return ref == nullptr;
  }

  bool operator==(const CoroutineHandle& other) const {
    return ref == other.ref;
  }

  bool operator==(std::coroutine_handle<> other) const {
    if (!*this) return other == nullptr;
    return handle == other;
  }

  operator bool () const {
    return (ref && ref->uses());
  }

  /// Resume the coroutine, updating `RunningCoroutine` in the process.
  ///
  /// If the coroutine has been destroyed, this method is a no-op.
  inline void resume() const;

  /// Construct a `CoroutineHandle` for the currently running coroutine.
  /// The returned value needs to be `co_await`ed (this will not actually
  /// suspend the coroutine).
  inline ObtainCoroutineHandleAwaiter getCurrent();
};

void PromiseRefcounter_::setParent(const CoroutineHandle& handle) {
  setParent(handle.ref);
}

class ObtainCoroutineHandleAwaiter {
  CoroutineHandle handle;
  friend class CoroutineHandle;
  ObtainCoroutineHandleAwaiter() = default;
public:
  bool await_ready() { return false; }
  template<class T>
  bool await_suspend(std::coroutine_handle<T> coro) {
    handle = {coro};
    return false;
  }
  CoroutineHandle await_resume() { return std::move(handle); }
};

inline ObtainCoroutineHandleAwaiter CoroutineHandle::getCurrent() { return {}; }

/// Keeps track of the currently running coroutine in a thread-local variable.
/// This is a bit of a hack, as the language does not offer a proper way to do
/// this. The current coroutine should be set when a coroutine is first started,
/// by the promise_type, and `CoroutineHandler::resume` should be used elsewhere.
///
/// When making a custom awaiter care should be taken to set the running
/// coroutine appropriately.
class RunningCoroutine {
  inline thread_local static CoroutineHandle currentCoroutine = {};
  RunningCoroutine() = delete;
public:
  static void set(CoroutineHandle coro) { currentCoroutine = coro; }
  static CoroutineHandle get() { return currentCoroutine; }
};

void CoroutineHandle::resume() const {
  if (!*this) return;
  struct Restorer {
    CoroutineHandle handle;
    Restorer(CoroutineHandle handle) : handle(handle) {}
    ~Restorer() { RunningCoroutine::set(handle); }
  } restorer(RunningCoroutine::get());
  RunningCoroutine::set(*this);
  handle.resume();
}

/// Internal object stored in coroutine frames, used for CancellationBlock.
///
/// When creating a `PromiseRefcount` for a coroutine, it is associated with
/// the currently running coroutine (normally the caller of an async function).
/// This forms a chain/tree of references, so that a CancellationBlock can
/// block the destruction of the entire async call chain.
class PromiseRefcount {
public:
  PromiseRefcounter_ *inner;

  template<class T>
  PromiseRefcount(T& obj) {
    inner = new PromiseRefcounter_(T::pref_deleter, &obj);
    inner->setParent(RunningCoroutine::get());
  }

  PromiseRefcount(const PromiseRefcount &) = delete;
  PromiseRefcount(PromiseRefcount &&) = delete;
  PromiseRefcount &operator=(const PromiseRefcount &) = delete;
  PromiseRefcount &operator=(PromiseRefcount &&) = delete;

  void release() {
    PromiseRefcounter_ *ptr = inner;
    inner = nullptr;
    if (ptr->release())
      delete ptr;
  }
};

} // namespace uvco
