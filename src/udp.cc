// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fmt/core.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <uv.h>

#include <boost/assert.hpp>
#include <fmt/format.h>

#include "close.h"
#include "exception.h"
#include "internal/internal_utils.h"
#include "name_resolution.h"
#include "promise/multipromise.h"
#include "promise/promise.h"
#include "run.h"
#include "udp.h"

#include <array>
#include <uv/unix.h>

namespace uvco {

Udp::~Udp() {
  if (is_receiving_) {
    fmt::print(stderr, "Udp::~Udp(): please co_await udp.stopReceiveMany() "
                       "before dropping Udp instance.\n");
  }
  if (udp_) {
    fmt::print(stderr, "Udp::~Udp(): closing UDP socket in dtor; "
                       "this will leak memory. "
                       "Please co_await udp.close() if possible.\n");
    udpStopReceive();
    closeHandle(udp_.release());
  }
}

Promise<void> Udp::bind(std::string_view address, uint16_t port,
                        unsigned int flag) {
  Resolver resolver{*loop_};
  int hint = 0;
  if ((flag & UV_UDP_IPV6ONLY) != 0U) {
    hint = AF_INET6;
  }
  AddressHandle addressHandle =
      co_await resolver.gai(address, fmt::format("{}", port), hint);

  co_await bind(addressHandle, flag);
}

Promise<void> Udp::bind(const AddressHandle &address, unsigned int flag) {
  uv_status status = uv_udp_bind(udp_.get(), address.sockaddr(), flag);
  if (status != 0) {
    co_await close();
    throw UvcoException{status, "binding UDP socket"};
  }
}

Promise<void> Udp::connect(std::string_view address, uint16_t port,
                           bool ipv6only) {
  Resolver resolver{*loop_};
  const int hint = ipv6only ? AF_INET6 : AF_UNSPEC;
  AddressHandle addressHandle = co_await resolver.gai(address, port, hint);

  uv_udp_connect(udp_.get(), nullptr);
  const uv_status status = uv_udp_connect(udp_.get(), addressHandle.sockaddr());
  if (status != 0) {
    co_await close();
    throw UvcoException{status, "connecting UDP socket"};
  }
  connected_ = true;
}

Promise<void> Udp::connect(const AddressHandle &address) {
  uv_udp_connect(udp_.get(), nullptr);
  const uv_status status = uv_udp_connect(udp_.get(), address.sockaddr());
  if (status != 0) {
    co_await close();
    throw UvcoException{status, "connecting UDP socket"};
  }
  connected_ = true;
}

Promise<void> Udp::send(std::span<char> buffer,
                        std::optional<AddressHandle> address) {
  SendAwaiter_ sendAwaiter{};
  uv_udp_send_t req;
  req.data = &sendAwaiter;

  std::array<uv_buf_t, 1> bufs{};
  // The buffer is never written to, so this is necessary to interface
  // with the legacy C code.
  bufs[0].base = &(*buffer.begin());
  bufs[0].len = buffer.size_bytes();

  const struct sockaddr *addr = nullptr;
  if (address) {
    addr = address->sockaddr();
  }

  uv_status status =
      uv_udp_send(&req, udp_.get(), bufs.begin(), 1, addr, onSendDone);
  if (status != 0) {
    throw UvcoException{status, "uv_udp_send() failed immediately"};
  }

  uv_status status_done = co_await sendAwaiter;
  if (status_done != 0) {
    throw UvcoException{status_done, "uv_udp_send() failed while sending"};
  }

  co_return;
}

Promise<std::string> Udp::receiveOne() {
  auto p = co_await receiveOneFrom();
  co_return std::move(p.first);
}

Promise<std::pair<std::string, AddressHandle>> Udp::receiveOneFrom() {
  RecvAwaiter_ awaiter{};
  udp_->data = &awaiter;
  uv_status status = udpStartReceive();
  if (status != 0) {
    throw UvcoException(status, "uv_udp_recv_start()");
  }

  std::optional<std::string> buffer = co_await awaiter;
  if (!buffer) {
    throw UvcoException(UV_EINTR, "receive process interrupted");
  }

  // Any exceptions are thrown in RecvAwaiter_::await_resume

  udp_->data = nullptr;
  BOOST_ASSERT(awaiter.addr_);
  co_return std::make_pair(*buffer, *awaiter.addr_);
}

MultiPromise<std::pair<std::string, AddressHandle>> Udp::receiveMany() {
  FlagGuard receivingGuard{is_receiving_};

  RecvAwaiter_ awaiter{};
  awaiter.stop_receiving_ = false;
  udp_->data = &awaiter;

  uv_status status = udpStartReceive();
  if (status != 0) {
    throw UvcoException(status, "receiveMany(): uv_udp_recv_start()");
  }

  while (uv_is_active((uv_handle_t *)udp_.get()) != 0) {
    // Awaiter returns empty optional on requested stop (stopReceiveMany()).
    std::optional<std::string> buffer = co_await awaiter;
    if (!buffer) {
      break;
    }
    BOOST_ASSERT(awaiter.addr_);
    // It's possible that co_yield doesn't resume anymore, therefore clear
    // reference to local awaiter.
    udp_->data = nullptr;
    co_yield std::make_pair(std::move(*buffer), *awaiter.addr_);
    udp_->data = &awaiter;
  }
  udp_->data = nullptr;
  co_return;
}

Promise<void> Udp::close() {
  BOOST_ASSERT(udp_);
  RecvAwaiter_ *awaiter = (RecvAwaiter_ *)udp_->data;
  if (awaiter != nullptr) {
    fmt::print(stderr, "Udp::close(): stopping receiving. Please instead use "
                       "Udp::stopReceivingMany() explicitly.\n");
    // Force return from receiveMany() generator.
    if (awaiter->handle_) {
      const auto resumeHandle = awaiter->handle_.value();
      awaiter->handle_.reset();
      resumeHandle.resume();
    }
  }
  co_await closeHandle(udp_.get());
  udp_.reset();
  connected_ = false;
}

void Udp::onReceiveOne(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                       const struct sockaddr *addr, unsigned int flags) {

  auto *awaiter = (RecvAwaiter_ *)handle->data;
  awaiter->nread_ = nread;

  if (addr == nullptr) {
    // Error or asking to free buffers.
    if (!(flags & UV_UDP_MMSG_CHUNK)) {
      freeUvBuf(buf);
    }
    return;
  }

  if (awaiter->stop_receiving_) {
    uv_udp_recv_stop(handle);
  }
  awaiter->addr_ = AddressHandle{addr};
  if (nread >= 0) {
    awaiter->buffer_ = std::string{buf->base, static_cast<size_t>(nread)};
  }

  if (0 == (flags & UV_UDP_MMSG_CHUNK)) {
    freeUvBuf(buf);
  }

  if (awaiter->handle_) {
    auto resumeHandle = *awaiter->handle_;
    awaiter->handle_.reset();
    Loop::enqueue(resumeHandle);
  }
}

bool Udp::RecvAwaiter_::await_suspend(std::coroutine_handle<> h) {
  BOOST_ASSERT(!handle_);
  handle_ = h;
  return true;
}

bool Udp::RecvAwaiter_::await_ready() const { return buffer_.has_value(); }

std::optional<std::string> Udp::RecvAwaiter_::await_resume() {
  // Woken up without read packet: stop receiving.
  if (!nread_ && !buffer_) {
    return {};
  }
  BOOST_ASSERT(nread_);
  if (*nread_ < 0) {
    throw UvcoException(*nread_,
                        "Udp::RecvAwaiter_::await_resume: error during recv");
  }
  BOOST_ASSERT(buffer_);
  auto b = std::move(*buffer_);
  buffer_.reset();
  return b;
}

void Udp::onSendDone(uv_udp_send_t *req, uv_status status) {
  auto *awaiter = (SendAwaiter_ *)req->data;
  awaiter->status_ = status;
  if (awaiter->handle_) {
    auto resumeHandle = *awaiter->handle_;
    awaiter->handle_.reset();
    Loop::enqueue(resumeHandle);
  }
}

bool Udp::SendAwaiter_::await_ready() const { return status_.has_value(); }

bool Udp::SendAwaiter_::await_suspend(std::coroutine_handle<> h) {
  BOOST_ASSERT(!handle_);
  handle_ = h;
  return true;
}

int Udp::SendAwaiter_::await_resume() {
  BOOST_ASSERT(status_);
  return *status_;
}

void Udp::stopReceiveMany(
    MultiPromise<std::pair<std::string, AddressHandle>> &packets) {
  udpStopReceive();
  // Cancel receiving generator if currently suspended by co_yield.
  packets.cancel();
  auto *currentAwaiter = (RecvAwaiter_ *)udp_->data;
  if (currentAwaiter == nullptr) {
    return;
  }
  currentAwaiter->nread_.reset();
  currentAwaiter->buffer_.reset();
  // If generator is suspended on co_await, resume it synchronously so it can
  // exit before the Udp instance is possibly destroyed.
  if (currentAwaiter->handle_) {
    // Don't schedule this on the event loop: we must synchronously terminate
    // the onReceiveMany() loop, otherwise it will exist after destruction of
    // the Udp instance and read invalid memory.
    currentAwaiter->handle_->resume();
    // Don't touch currentAwaiter after this!!
  }
}

void Udp::udpStopReceive() {
  BOOST_ASSERT(udp_);
  uv_udp_recv_stop(udp_.get());
}

int Udp::udpStartReceive() {
  BOOST_ASSERT(udp_);
  return uv_udp_recv_start(udp_.get(), allocator, onReceiveOne);
}

void Udp::setBroadcast(bool enabled) {
  uv_status status =
      uv_udp_set_broadcast(udp_.get(), static_cast<int>(enabled));
  if (status != 0) {
    throw UvcoException(status, "join multicast group");
  }
}

void Udp::setTtl(uint8_t ttl) {
  uv_status status = uv_udp_set_ttl(udp_.get(), static_cast<int>(ttl));
  if (status != 0) {
    throw UvcoException(status, "join multicast group");
  }
}

void Udp::setMulticastInterface(const std::string &interfaceAddress) {
  uv_status status =
      uv_udp_set_multicast_interface(udp_.get(), interfaceAddress.c_str());
  if (status != 0) {
    throw UvcoException(status, "join multicast group");
  }
}

void Udp::setMulticastLoop(bool enabled) {
  uv_status status =
      uv_udp_set_multicast_loop(udp_.get(), static_cast<int>(enabled));
  if (status != 0) {
    throw UvcoException(status, "join multicast group");
  }
}

void Udp::joinMulticast(const std::string &address,
                        const std::string &interface) {
  uv_status status = uv_udp_set_membership(udp_.get(), address.c_str(),
                                           interface.c_str(), UV_JOIN_GROUP);
  if (status != 0) {
    throw UvcoException(status, "join multicast group");
  }
}

void Udp::leaveMulticast(const std::string &address,
                         const std::string &interface) {
  uv_status status = uv_udp_set_membership(udp_.get(), address.c_str(),
                                           interface.c_str(), UV_LEAVE_GROUP);
  if (status != 0) {
    throw UvcoException(status, "join multicast group");
  }
}

AddressHandle Udp::getSockname() const {
  struct sockaddr_storage address;
  int ss_size = sizeof(struct sockaddr_storage);
  uv_status status =
      uv_udp_getsockname(udp_.get(), (struct sockaddr *)&address, &ss_size);
  if (status < 0) {
    throw UvcoException(status, "Error in getsockname");
  }
  AddressHandle addressHandle{(struct sockaddr *)&address};
  return addressHandle;
}

std::optional<AddressHandle> Udp::getPeername() const {
  struct sockaddr_storage address;
  int ss_size = sizeof(struct sockaddr_storage);
  uv_status status =
      uv_udp_getpeername(udp_.get(), (struct sockaddr *)&address, &ss_size);
  if (status < 0) {
    if (status == UV_ENOTCONN) {
      return {};
    }
    throw UvcoException(status, "Error in getpeername");
  }
  AddressHandle addressHandle{(struct sockaddr *)&address};
  return addressHandle;
}

uv_udp_t *Udp::underlying() const { return udp_.get(); }

} // namespace uvco
