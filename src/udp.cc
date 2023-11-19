// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <boost/assert.hpp>

#include "close.h"
#include "udp.h"

namespace uvco {

Promise<void> Udp::bind(std::string_view address, uint16_t port,
                        unsigned int flag) {
  Resolver resolver{loop_};
  int hint = 0;
  if ((flag & UV_UDP_IPV6ONLY) != 0U) {
    hint = AF_INET6;
  }
  AddressHandle addressHandle =
      co_await resolver.gai(address, fmt::format("{}", port), hint);

  int status = uv_udp_bind(udp_.get(), addressHandle.sockaddr(), flag);
  if (status != 0)
    throw UvcoException{status, "binding UDP socket"};
}

Promise<void> Udp::connect(std::string_view address, uint16_t port,
                           bool ipv6only) {
  Resolver r{loop_};
  int hint = ipv6only ? AF_INET6 : AF_UNSPEC;
  AddressHandle addressHandle =
      co_await r.gai(address, fmt::format("{}", port), hint);

  int status = uv_udp_connect(udp_.get(), addressHandle.sockaddr());
  if (status != 0) {

    throw UvcoException{status, "connecting UDP socket"};
  }
  connected_ = true;
}

Promise<std::string> Udp::receiveOne() {
  auto p = co_await receiveOneFrom();
  co_return std::move(p.first);
}

Promise<std::pair<std::string, AddressHandle>> Udp::receiveOneFrom() {
  RecvAwaiter_ awaiter{};
  udp_->data = &awaiter;
  int status = uv_udp_recv_start(udp_.get(), allocator, onReceiveOne);
  if (status != 0)
    throw UvcoException(status, "uv_udp_recv_start()");

  std::string buffer = co_await awaiter;

  // Any exceptions are thrown in RecvAwaiter_::await_resume

  udp_->data = nullptr;
  BOOST_ASSERT(awaiter.addr_);
  co_return std::make_pair(buffer, *awaiter.addr_);
}

MultiPromise<std::pair<std::string, AddressHandle>> Udp::receiveMany() {
  RecvAwaiter_ awaiter{};
  awaiter.stop_receiving_ = false;
  udp_->data = &awaiter;

  int status = uv_udp_recv_start(udp_.get(), allocator, onReceiveOne);
  if (status != 0) {
    throw UvcoException(status, "receiveMany(): uv_udp_recv_start()");
  }

  while (true) {
    std::string buffer = co_await awaiter;
    BOOST_ASSERT(awaiter.addr_);
    co_yield std::make_pair(std::move(buffer), *awaiter.addr_);
    // TODO: a way to stop?
  };
  co_return;
}

Promise<void> Udp::close() {
  co_await closeHandle(udp_.get());
  udp_.reset();
  connected_ = false;
}

void Udp::onReceiveOne(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                       const struct sockaddr *addr, unsigned int flags) {

  auto *awaiter = (RecvAwaiter_ *)handle->data;
  if (awaiter->stop_receiving_) {
    uv_udp_recv_stop(handle);
  }
  awaiter->nread_ = nread;

  if (addr == nullptr) {
    // Error or asking to free buffers.
    if (!(flags & UV_UDP_MMSG_CHUNK)) {
      freeUvBuf(buf);
    }
    return;
  } else {
    awaiter->addr_ = AddressHandle{addr};
    if (nread >= 0) {
      awaiter->buffer_ = std::string{buf->base, static_cast<size_t>(nread)};
    }
  }

  if (0 == (flags & UV_UDP_MMSG_CHUNK)) {
    freeUvBuf(buf);
  }

  if (awaiter->handle_) {
    auto handle = *awaiter->handle_;
    awaiter->handle_.reset();
    handle.resume();
  }
}

bool Udp::RecvAwaiter_::await_suspend(std::coroutine_handle<> h) {
  BOOST_ASSERT(!handle_);
  handle_ = h;
  return true;
}

bool Udp::RecvAwaiter_::await_ready() const { return buffer_.has_value(); }

std::string Udp::RecvAwaiter_::await_resume() {
  BOOST_ASSERT(nread_);
  if (*nread_ < 0) {
    throw UvcoException(*nread_, "onReceiveOne");
  }
  BOOST_ASSERT(buffer_);
  auto b = std::move(*buffer_);
  buffer_.reset();
  return b;
}

void Udp::onSendDone(uv_udp_send_t *req, int status) {
  auto *awaiter = (SendAwaiter_ *)req->data;
  awaiter->status_ = status;
  if (awaiter->handle_) {
    awaiter->handle_->resume();
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

} // namespace uvco
