// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "exception.h"
#include "promise.h"

#include <uv.h>

#include <coroutine>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace uvco {

/// @addtogroup DNS
/// @{
/// DNS and name resolution is supported by means of `AddressHandle`, a light-weight but convenient
/// wrapper around numeric TCP/IP addresses, and `Resolver`, which enables asynchronous DNS
/// lookups.

/// @brief AddressHandle is a light-weight wrapper around a `struct sockaddr_in(6)`, and is therefore
/// cheap to copy.
///
/// It can be constructed from different forms of TCP/IP addresses, and also supports
/// formatting an address to a string.
///
/// In order to resolve a DNS hostname, use the `Resolver` class.
class AddressHandle {
  struct NtopHelper_;

public:
  constexpr static size_t ipv4Length = 4;
  constexpr static size_t ipv6Length = 16;

  AddressHandle() = default;
  AddressHandle(const AddressHandle &) = default;
  AddressHandle(AddressHandle &&) = default;
  AddressHandle &operator=(const AddressHandle &) = default;
  AddressHandle &operator=(AddressHandle &&) = default;
  ~AddressHandle() = default;

  AddressHandle(std::span<const uint8_t> ipv4_or_6, uint16_t port,
                uint32_t v6scope = 0);
  AddressHandle(uint32_t ipv4, uint16_t port);
  AddressHandle(std::string_view ip, uint16_t port, uint32_t v6scope = 0);
  explicit AddressHandle(const struct addrinfo *ai);
  explicit AddressHandle(const struct sockaddr *sa);

  // The address, formatted by `inet_ntop(3)`.
  std::string address() const { return std::visit(NtopHelper_{}, addr_); }
  uint16_t port() const;
  /// Family is either `AF_INET` or `AF_INET6`.
  int family() const;
  /// The inner sockaddr struct. May refer to either a `sockaddr_in` or a
  /// `sockaddr_in6`, depending on `family()`.
  const struct sockaddr *sockaddr() const;

  std::string toString() const;

private:
  std::variant<struct sockaddr_in, struct sockaddr_in6> addr_{};

  /// A helper for calling `inet_ntop(3)`.
  struct NtopHelper_ {
    std::string operator()(const struct sockaddr_in &ipv4);
    std::string operator()(const struct sockaddr_in6 &ipv6);
    std::string ntop(int family, void *addr);
  };
};

/// Asynchronous name resolution using the `libuv` `getaddrinfo(3)` interface.
class Resolver {
  struct AddrinfoAwaiter_;

public:
  /// Instantiate a resolver based on an event loop.
  explicit Resolver(uv_loop_t *loop) : loop_{loop} {}

  /// Resolve a host and port string. Throws an `UvcoException` upon error.
  Promise<AddressHandle> gai(std::string_view host, std::string_view port,
                             int af_hint = AF_UNSPEC);
  /// Resolve a host string and numeric port. Throws an `UvcoException` upon error.
  Promise<AddressHandle> gai(std::string_view host, uint16_t port,
                             int af_hint = AF_UNSPEC);

private:
  uv_loop_t *loop_;

  static void onAddrinfo(uv_getaddrinfo_t *req, uv_status status,
                         struct addrinfo *result);

  struct AddrinfoAwaiter_ {
    AddrinfoAwaiter_() : req_{} {}
    bool await_ready() const { return false; }
    bool await_suspend(std::coroutine_handle<> handle);

    struct addrinfo *await_resume();

    uv_getaddrinfo_t req_;
    std::optional<struct addrinfo *> addrinfo_;
    std::optional<int> status_;
    std::optional<std::coroutine_handle<>> handle_;
  };
};

/// @}

} // namespace uvco
