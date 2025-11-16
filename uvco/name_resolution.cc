// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <memory>
#include <uv.h>

#include "exception.h"
#include "internal/internal_utils.h"
#include "name_resolution.h"
#include "promise/promise.h"
#include "run.h"

#include <algorithm>
#include <arpa/inet.h>
#include <boost/assert.hpp>
#include <cerrno>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <fmt/core.h>
#include <netdb.h>
#include <netinet/in.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <variant>

namespace uvco {

AddressHandle::AddressHandle(std::span<const uint8_t> ipv4_or_6, uint16_t port,
                             uint32_t v6scope) {
  if (ipv4_or_6.size() == ipv4Length) {
    struct sockaddr_in addr{};
    struct in_addr ipAddr{};
    ipAddr.s_addr = *(uint32_t *)ipv4_or_6.data();

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = ipAddr;
    addr_ = addr;
  } else if (ipv4_or_6.size() == ipv6Length) {
    struct sockaddr_in6 addr{};
    struct in6_addr ipAddr{};

    std::ranges::copy(ipv4_or_6, static_cast<uint8_t *>(ipAddr.s6_addr));

    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    addr.sin6_addr = ipAddr;
    addr.sin6_scope_id = v6scope;
    addr_ = addr;
  } else {
    throw UvcoException("Invalid address size for IPv4/6 address!");
  }
}

std::string AddressHandle::toString() const {
  if (family() == AF_INET) {
    return fmt::format("{}:{}", address(), port());
  }
  if (family() == AF_INET6) {
    return fmt::format("[{}]:{}", address(), port());
  }
  return {};
}
uint16_t AddressHandle::port() const {
  if (addr_.index() == 0) {
    const auto &addr = std::get<0>(addr_);
    return ntohs(addr.sin_port);
  } else {
    const auto &addr = std::get<1>(addr_);
    return ntohs(addr.sin6_port);
  }
}
int AddressHandle::family() const {
  if (addr_.index() == 0)
    return AF_INET;
  if (addr_.index() == 1)
    return AF_INET6;
  throw UvcoException("family(): unknown address variant!");
}
const struct sockaddr *AddressHandle::sockaddr() const {
  return std::visit(
      [](const auto &sockaddr) -> const struct sockaddr * {
        return (const struct sockaddr *)&sockaddr;
      },
      addr_);
}

AddressHandle::AddressHandle(std::string_view ip, uint16_t port,
                             uint32_t v6scope) {
  if (ip.contains(':')) {
    struct in6_addr ipAddr{};
    uv_status status = inet_pton(AF_INET6, ip.data(), &ipAddr);
    if (status != 1) {
      throw UvcoException(fmt::format("invalid IPv6 address: {}", ip));
    }

    struct sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = ipAddr;
    addr.sin6_port = htons(port);
    addr.sin6_scope_id = v6scope;
    addr_ = addr;
  } else {
    struct in_addr ipAddr;
    uv_status status = inet_pton(AF_INET, ip.data(), &ipAddr);
    if (status != 1) {
      throw UvcoException(fmt::format("invalid IPv4 address: {}", ip));
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr = ipAddr;
    addr.sin_port = htons(port);
    addr_ = addr;
  }
}

AddressHandle::AddressHandle(uint32_t ipv4, uint16_t port)
    : AddressHandle{std::span<const uint8_t>{(uint8_t *)(&ipv4), 4}, port} {}

AddressHandle::AddressHandle(const struct addrinfo *ai) {
  if (ai->ai_family == AF_INET) {
    BOOST_ASSERT(ai->ai_addrlen >= sizeof(struct sockaddr_in));
    addr_ = *(struct sockaddr_in *)ai->ai_addr;
  } else if (ai->ai_family == AF_INET6) {
    BOOST_ASSERT(ai->ai_addrlen >= sizeof(struct sockaddr_in6));
    addr_ = *(struct sockaddr_in6 *)ai->ai_addr;
  }
}

AddressHandle::AddressHandle(const struct sockaddr *sa) {
  int af = sa->sa_family;
  if (af == AF_INET) {
    const auto *addr = (struct sockaddr_in *)sa;
    addr_ = *addr;
  } else if (af == AF_INET6) {
    const auto *addr = (struct sockaddr_in6 *)sa;
    addr_ = *addr;
  } else {
    throw UvcoException(fmt::format("unknown address family {}", af));
  }
}

std::string AddressHandle::NtopHelper_::ntop(int family, void *addr) {
  std::string dst{};
  if (family == AF_INET) {
    dst.resize(4 * 3 + 3 + 1);
  } else if (family == AF_INET6) {
    dst.resize(8 * 4 + 7 + 1);
  }
  const char *result = inet_ntop(family, addr, dst.data(), dst.size());
  if (result == nullptr) {
    throw UvcoException(fmt::format("inet_ntop(): {}", std::strerror(errno)));
  }
  dst.resize(std::strlen(result));
  return dst;
}

std::string
AddressHandle::NtopHelper_::operator()(const struct sockaddr_in6 &ipv6) {
  return ntop(ipv6.sin6_family, (void *)&ipv6.sin6_addr);
}

std::string
AddressHandle::NtopHelper_::operator()(const struct sockaddr_in &ipv4) {
  return ntop(ipv4.sin_family, (void *)&ipv4.sin_addr);
}

namespace {

struct AddrinfoAwaiter_ {
  AddrinfoAwaiter_();
  ~AddrinfoAwaiter_();

  bool await_ready();
  bool await_suspend(std::coroutine_handle<> handle);

  struct addrinfo *await_resume();

  // must be unique_ptr to support legal cancellation of lookups.
  std::unique_ptr<uv_getaddrinfo_t> req_;
  std::optional<struct addrinfo *> addrinfo_;
  std::optional<int> status_;
  std::coroutine_handle<> handle_;
};

AddrinfoAwaiter_::AddrinfoAwaiter_()
    : req_{std::make_unique<uv_getaddrinfo_t>()} {}

AddrinfoAwaiter_::~AddrinfoAwaiter_() {
  if (req_ != nullptr && !requestDataIsNull(req_.get())) {
    resetRequestData(req_.get());
    // Request will be freed by onAddrinfo callback
    uv_cancel((uv_req_t *)req_.release());
  }
  if (addrinfo_.has_value()) {
    uv_freeaddrinfo(addrinfo_.value());
  }
}

bool AddrinfoAwaiter_::await_ready() { return false; }

struct addrinfo *AddrinfoAwaiter_::await_resume() {
  BOOST_ASSERT(addrinfo_);
  BOOST_ASSERT(!requestDataIsNull(req_.get()));
  resetRequestData(req_.get());
  return *addrinfo_;
}

bool AddrinfoAwaiter_::await_suspend(std::coroutine_handle<> handle) {
  handle_ = handle;
  setRequestData(req_.get(), this);
  return true;
}

} // namespace

Promise<AddressHandle> Resolver::gai(std::string_view host, uint16_t port,
                                     int af_hint) {
  const std::string portStr = std::to_string(port);
  co_return (co_await gai(host, portStr, af_hint));
}

Promise<AddressHandle> Resolver::gai(std::string_view host,
                                     std::string_view port, int af_hint) {
  AddrinfoAwaiter_ awaiter;
  struct addrinfo hints{};
  hints.ai_family = af_hint;
  hints.ai_socktype = SOCK_STREAM;

  uv_getaddrinfo(loop_->uvloop(), awaiter.req_.get(), onAddrinfo, host.data(),
                 port.data(), &hints);
  // Npte: we rely on libuv not resuming before awaiting the result.
  struct addrinfo *result = co_await awaiter;

  const uv_status status = awaiter.status_.value();
  if (status != 0) {
    throw UvcoException{status, "getaddrinfo()"};
  }

  co_return AddressHandle{result};
}

void Resolver::onAddrinfo(uv_getaddrinfo_t *req, uv_status status,
                          struct addrinfo *result) {
  // asserts request data not null; the only time this could be the case is
  // after a lookup is cancelled.
  auto *awaiter = getRequestDataOrNull<AddrinfoAwaiter_>(req);
  if (awaiter == nullptr) {
    // cancelled
    delete req;
    uv_freeaddrinfo(result);
    return;
  }
  if (status == UV_ECANCELED) {
    return;
  }
  awaiter->addrinfo_ = result;
  awaiter->status_ = status;
  BOOST_ASSERT(awaiter->handle_ != nullptr);
  Loop::enqueue(awaiter->handle_);
}

} // namespace uvco
