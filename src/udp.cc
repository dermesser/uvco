// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <boost/assert.hpp>

#include "close.h"
#include "internal_utils.h"
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

Promise<void> Udp::send(std::span<char> buffer,
                        std::optional<AddressHandle> address) {
  SendAwaiter_ sendAwaiter{};
  uv_udp_send_t req;
  req.data = &sendAwaiter;

  std::array<uv_buf_t, 1> bufs{};
  bufs[0].base = &(*buffer.begin());
  bufs[0].len = buffer.size_bytes();

  const struct sockaddr *addr = nullptr;
  if (address) {
    addr = address->sockaddr();
  }

  int status = uv_udp_send(&req, udp_.get(), bufs.begin(), 1, addr, onSendDone);
  if (status != 0) {
    throw UvcoException{status, "uv_udp_send() failed immediately"};
  }

  int status_done = co_await sendAwaiter;
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
  int status = uv_udp_recv_start(udp_.get(), allocator, onReceiveOne);
  if (status != 0) {
    throw UvcoException(status, "uv_udp_recv_start()");
}

  std::string buffer = co_await awaiter;

  // Any exceptions are thrown in RecvAwaiter_::await_resume

  udp_->data = nullptr;
  BOOST_ASSERT(awaiter.addr_);
  co_return std::make_pair(buffer, *awaiter.addr_);
}

MultiPromise<std::pair<std::string, AddressHandle>> Udp::receiveMany() {
  FlagGuard receivingGuard{is_receiving_};

  RecvAwaiter_ awaiter{};
  awaiter.stop_receiving_ = false;
  udp_->data = &awaiter;

  int status = uv_udp_recv_start(udp_.get(), allocator, onReceiveOne);
  if (status != 0) {
    throw UvcoException(status, "receiveMany(): uv_udp_recv_start()");
  }

  stop_receive_many_ = false;
  while (!stop_receive_many_) {
    std::string buffer = co_await awaiter;
    BOOST_ASSERT(awaiter.addr_);
    co_yield std::make_pair(std::move(buffer), *awaiter.addr_);
  };
  stop_receive_many_ = false;
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
