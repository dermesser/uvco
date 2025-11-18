// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <boost/assert.hpp>
#include <uv.h>
#include <uv/unix.h>

#include "uvco/bounded_queue.h"
#include "uvco/close.h"
#include "uvco/exception.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/name_resolution.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"
#include "uvco/udp.h"
#include "uvco/util.h"

#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fmt/core.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <variant>

namespace uvco {

struct Udp::RecvAwaiter_ {
  static constexpr unsigned packetQueueSize = 128;
  using QueueItem_ =
      std::variant<std::pair<std::string, AddressHandle>, uv_status>;

  explicit RecvAwaiter_(uv_udp_t &udp, size_t queueSize = packetQueueSize);
  RecvAwaiter_(const RecvAwaiter_ &) = default;
  RecvAwaiter_(RecvAwaiter_ &&) = delete;
  RecvAwaiter_ &operator=(const RecvAwaiter_ &) = delete;
  RecvAwaiter_ &operator=(RecvAwaiter_ &&) = delete;
  ~RecvAwaiter_() = default;

  [[nodiscard]] bool await_ready() const;
  bool await_suspend(std::coroutine_handle<> handle);
  std::optional<std::pair<std::string, AddressHandle>> await_resume();

  uv_udp_t &udp_;
  BoundedQueue<QueueItem_> buffer_;
  std::optional<std::coroutine_handle<>> handle_;
  bool stop_receiving_ = true;
};

struct Udp::SendAwaiter_ {
  explicit SendAwaiter_(uv_udp_send_t &req) : req_{req} {}
  SendAwaiter_(const SendAwaiter_ &) = default;
  SendAwaiter_(SendAwaiter_ &&) = delete;
  SendAwaiter_ &operator=(const SendAwaiter_ &) = delete;
  SendAwaiter_ &operator=(SendAwaiter_ &&) = delete;
  ~SendAwaiter_() { resetData(&req_); }

  [[nodiscard]] bool await_ready() const;
  bool await_suspend(std::coroutine_handle<> h);
  int await_resume();

  uv_udp_send_t &req_;
  std::optional<std::coroutine_handle<>> handle_;
  std::optional<int> status_;
};

Udp::Udp(const Loop &loop) : loop_{&loop}, udp_{std::make_unique<uv_udp_t>()} {
  uv_udp_init(loop.uvloop(), udp_.get());
}

Udp::~Udp() {
  if (udp_) {
    udpStopReceive();
    closeHandle(udp_.release());
  }
}

Promise<void> Udp::bind(std::string_view address, uint16_t port,
                        unsigned flag) {
  Resolver resolver{*loop_};
  int hint = 0;
  if ((flag & UV_UDP_IPV6ONLY) != 0U) {
    hint = AF_INET6;
  }
  AddressHandle addressHandle =
      co_await resolver.gai(address, std::to_string(port), hint);

  co_await bind(addressHandle, flag);
}

Promise<void> Udp::bind(const AddressHandle &address, unsigned int flag) {
  const uv_status status = uv_udp_bind(udp_.get(), address.sockaddr(), flag);
  if (status != 0) {
    close();
    throw UvcoException{status, "binding UDP socket"};
  }
  co_return;
}

Promise<void> Udp::connect(std::string_view address, uint16_t port,
                           bool ipv6only) {
  Resolver resolver{*loop_};
  const int hint = ipv6only ? AF_INET6 : AF_UNSPEC;
  AddressHandle addressHandle = co_await resolver.gai(address, port, hint);

  uv_udp_connect(udp_.get(), nullptr);
  const uv_status status = uv_udp_connect(udp_.get(), addressHandle.sockaddr());
  if (status != 0) {
    close();
    throw UvcoException{status, "connecting UDP socket"};
  }
  connected_ = true;
  co_return;
}

Promise<void> Udp::connect(const AddressHandle &address) {
  uv_udp_connect(udp_.get(), nullptr);
  const uv_status status = uv_udp_connect(udp_.get(), address.sockaddr());
  if (status != 0) {
    close();
    throw UvcoException{status, "connecting UDP socket"};
  }
  connected_ = true;
  co_return;
}

Promise<void> Udp::send(std::span<const char> buffer,
                        std::optional<AddressHandle> address) {
  uv_udp_send_t req{};
  SendAwaiter_ sendAwaiter{req};

  std::array<uv_buf_t, 1> bufs{};
  // The buffer is never written to, so this is necessary to interface
  // with the legacy C code.
  bufs[0] = uv_buf_init(const_cast<char *>(buffer.data()), buffer.size_bytes());

  const struct sockaddr *addr = nullptr;
  if (address) {
    addr = address->sockaddr();
  }

  const uv_status status =
      uv_udp_send(&req, udp_.get(), bufs.begin(), 1, addr, onSendDone);
  if (status != 0) {
    throw UvcoException{status, "uv_udp_send() failed immediately"};
  }

  const uv_status status_done = co_await sendAwaiter;
  if (status_done != 0) {
    throw UvcoException{status_done, "uv_udp_send() failed while sending"};
  }

  co_return;
}

Promise<std::string> Udp::receiveOne() {
  std::pair<std::basic_string<char>, AddressHandle> packet =
      co_await receiveOneFrom();
  co_return std::move(packet.first);
}

Promise<std::pair<std::string, AddressHandle>> Udp::receiveOneFrom() {
  BOOST_ASSERT_MSG(dataIsNull(udp_.get()),
                   "only one coroutine can receive from UDP socket at a time");
  RecvAwaiter_ awaiter{*udp_, 1};
  awaiter.stop_receiving_ = true;
  const OnExit onExit{[this] { resetData(udp_.get()); }};

  const uv_status status = udpStartReceive();
  if (status != 0) {
    throw UvcoException(status, "uv_udp_recv_start()");
  }

  // Exception thrown here if occurred.
  std::optional<std::pair<std::string, AddressHandle>> packet =
      co_await awaiter;

  // Any exceptions are thrown in RecvAwaiter_::await_resume
  co_return std::move(packet.value());
}

MultiPromise<std::pair<std::string, AddressHandle>> Udp::receiveMany() {
  BOOST_ASSERT_MSG(dataIsNull(udp_.get()),
                   "only one coroutine can receive from UDP socket at a time");
  RecvAwaiter_ awaiter{*udp_};
  awaiter.stop_receiving_ = false;
  const OnExit onExit{[this] {
    if (udp_) {
      udpStopReceive();
      resetData(udp_.get());
    }
  }};

  const uv_status status = udpStartReceive();
  if (status != 0) {
    throw UvcoException(status, "receiveMany(): uv_udp_recv_start()");
  }

  while (uv_is_active((uv_handle_t *)udp_.get()) != 0) {
    // Awaiter returns empty optional on requested stop (stopReceiveMany()).
    std::optional<std::pair<std::string, AddressHandle>> buffer =
        co_await awaiter;
    if (!buffer) {
      break;
    }
    co_yield std::move(buffer.value());
  }
  co_return;
}

void Udp::close() {
  if (udp_ == nullptr) {
    return;
  }
  if (!dataIsNull(udp_.get())) {
    auto *const awaiter = getData<RecvAwaiter_>(udp_.get());
    awaiter->buffer_.put(uv_status{UV_ECANCELED});
    if (awaiter->handle_) {
      // Loop::enqueue(awaiter->handle_.value());
      const std::coroutine_handle<> h = awaiter->handle_.value();
      awaiter->handle_.reset();
      // This immediately resumes the receiveMany generator which will get the
      // ECANCELED exception, store it, and make it available to anyone waiting
      // on it.
      h.resume();
    }
  }
  closeHandle(udp_.release());
  connected_ = false;
}

void Udp::stopReceiveMany(
    MultiPromise<std::pair<std::string, AddressHandle>> &packets) {
  udpStopReceive();
  // Cancel receiving generator if currently suspended by co_yield.
  packets.cancel();
}

void Udp::udpStopReceive() {
  BOOST_ASSERT(udp_);
  uv_udp_recv_stop(udp_.get());
}

int Udp::udpStartReceive() {
  BOOST_ASSERT(udp_);
  return uv_udp_recv_start(udp_.get(), allocator, onReceiveOne);
}

void Udp::onReceiveOne(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                       const struct sockaddr *addr, unsigned int flags) {
  auto *awaiter = getDataOrNull<RecvAwaiter_>(handle);
  // cancelled?
  if (awaiter == nullptr) {
    uv_udp_recv_stop(handle);
    freeUvBuf(buf);
    return;
  }

  if (addr == nullptr) {
    // Error or asking to free buffers.
    if (0 == (flags & UV_UDP_MMSG_CHUNK)) {
      freeUvBuf(buf);
    }
    return;
  }

  if (awaiter->stop_receiving_) {
    uv_udp_recv_stop(handle);
  }
  if (awaiter->buffer_.hasSpace()) {
    if (nread >= 0) {
      awaiter->buffer_.put(RecvAwaiter_::QueueItem_{
          std::make_pair(std::string{buf->base, static_cast<size_t>(nread)},
                         AddressHandle{addr})});
    } else {
      awaiter->buffer_.put(
          RecvAwaiter_::QueueItem_{static_cast<uv_status>(nread)});
    }
  } else {
    fmt::print(stderr, "Udp::onReceiveOne: dropping packet, buffer full\n");
  }

  if (0 == (flags & UV_UDP_MMSG_CHUNK)) {
    freeUvBuf(buf);
  }

  // Only enqueues once; if this callback is called again, the receiver will
  // already have been resumed.
  if (awaiter->handle_) {
    std::coroutine_handle<void> resumeHandle = *awaiter->handle_;
    awaiter->handle_.reset();
    Loop::enqueue(resumeHandle);
  }
}

void Udp::setBroadcast(bool enabled) {
  const uv_status status =
      uv_udp_set_broadcast(udp_.get(), static_cast<int>(enabled));
  if (status != 0) {
    throw UvcoException(status, "join multicast group");
  }
}

void Udp::setTtl(uint8_t ttl) {
  const uv_status status = uv_udp_set_ttl(udp_.get(), static_cast<int>(ttl));
  if (status != 0) {
    throw UvcoException(status, "join multicast group");
  }
}

void Udp::setMulticastInterface(const std::string &interfaceAddress) {
  const uv_status status =
      uv_udp_set_multicast_interface(udp_.get(), interfaceAddress.c_str());
  if (status != 0) {
    throw UvcoException(status, "join multicast group");
  }
}

void Udp::setMulticastLoop(bool enabled) {
  const uv_status status =
      uv_udp_set_multicast_loop(udp_.get(), static_cast<int>(enabled));
  if (status != 0) {
    throw UvcoException(status, "join multicast group");
  }
}

void Udp::joinMulticast(const std::string &address,
                        const std::string &interface) {
  const uv_status status = uv_udp_set_membership(
      udp_.get(), address.c_str(), interface.c_str(), UV_JOIN_GROUP);
  if (status != 0) {
    throw UvcoException(status, "join multicast group");
  }
}

void Udp::leaveMulticast(const std::string &address,
                         const std::string &interface) {
  const uv_status status = uv_udp_set_membership(
      udp_.get(), address.c_str(), interface.c_str(), UV_LEAVE_GROUP);
  if (status != 0) {
    throw UvcoException(status, "join multicast group");
  }
}

AddressHandle Udp::getSockname() const {
  struct sockaddr_storage address{};
  int ss_size = sizeof(struct sockaddr_storage);
  const uv_status status =
      uv_udp_getsockname(udp_.get(), (struct sockaddr *)&address, &ss_size);
  if (status < 0) {
    throw UvcoException(status, "Error in getsockname");
  }
  AddressHandle addressHandle{(struct sockaddr *)&address};
  return addressHandle;
}

std::optional<AddressHandle> Udp::getPeername() const {
  struct sockaddr_storage address{};
  int ss_size = sizeof(struct sockaddr_storage);
  const uv_status status =
      uv_udp_getpeername(udp_.get(), (struct sockaddr *)&address, &ss_size);
  if (status < 0) {
    if (status == UV_ENOTCONN) {
      return std::nullopt;
    }
    throw UvcoException(status, "Error in getpeername");
  }
  AddressHandle addressHandle{(struct sockaddr *)&address};
  return addressHandle;
}

Udp::RecvAwaiter_::RecvAwaiter_(uv_udp_t &udp, size_t queueSize)
    : udp_{udp}, buffer_{queueSize} {}

bool Udp::RecvAwaiter_::await_suspend(std::coroutine_handle<> handle) {
  BOOST_ASSERT(dataIsNull(&udp_));
  setData(&udp_, this);
  BOOST_ASSERT(!handle_);
  handle_ = handle;
  return true;
}

bool Udp::RecvAwaiter_::await_ready() const { return !buffer_.empty(); }

std::optional<std::pair<std::string, AddressHandle>>
Udp::RecvAwaiter_::await_resume() {
  resetData(&udp_);
  // Woken up without read packet: stop receiving.
  if (buffer_.empty()) {
    return std::nullopt;
  }
  QueueItem_ item = buffer_.get();
  if (std::holds_alternative<uv_status>(item)) {
    throw UvcoException(std::get<uv_status>(item),
                        "Udp::RecvAwaiter_::await_resume: error during recv");
  }
  return std::get<std::pair<std::string, AddressHandle>>(item);
}

void Udp::onSendDone(uv_udp_send_t *req, uv_status status) {
  auto *const awaiter = getRequestDataOrNull<SendAwaiter_>(req);
  if (awaiter == nullptr) {
    // Send request cancelled/dropped.
    return;
  }
  awaiter->status_ = status;
  if (awaiter->handle_) {
    std::coroutine_handle<void> resumeHandle = *awaiter->handle_;
    awaiter->handle_.reset();
    Loop::enqueue(resumeHandle);
  }
}

bool Udp::SendAwaiter_::await_ready() const { return status_.has_value(); }

bool Udp::SendAwaiter_::await_suspend(std::coroutine_handle<> handle) {
  setData(&req_, this);
  BOOST_ASSERT(!handle_);
  handle_ = handle;
  return true;
}

int Udp::SendAwaiter_::await_resume() {
  BOOST_ASSERT(status_);
  return *status_;
}

} // namespace uvco
