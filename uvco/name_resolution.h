// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <uv.h>

#include "uvco/internal/internal_utils.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace uvco {

/// @addtogroup DNS
/// @{
/// DNS and name resolution is supported by means of `AddressHandle`, a
/// light-weight but convenient wrapper around numeric TCP/IP addresses, and
/// `Resolver`, which enables asynchronous DNS lookups.

/// @brief AddressHandle is a light-weight wrapper around a `struct
/// sockaddr_in(6)`, and is therefore cheap to copy.
///
/// It can be constructed from different forms of TCP/IP addresses, and also
/// supports formatting an address to a string.
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
  [[nodiscard]] std::string address() const {
    return std::visit(NtopHelper_{}, addr_);
  }
  [[nodiscard]] uint16_t port() const;
  /// Family is either `AF_INET` or `AF_INET6`.
  int family() const;
  /// The inner sockaddr struct. May refer to either a `sockaddr_in` or a
  /// `sockaddr_in6`, depending on `family()`.
  [[nodiscard]] const struct sockaddr *sockaddr() const;

  [[nodiscard]] std::string toString() const;

private:
  std::variant<struct sockaddr_in, struct sockaddr_in6> addr_;

  /// A helper for calling `inet_ntop(3)`.
  struct NtopHelper_ {
    static std::string operator()(const struct sockaddr_in &ipv4);
    static std::string operator()(const struct sockaddr_in6 &ipv6);
    static std::string ntop(int family, void *addr);
  };
};

/// Asynchronous name resolution using the `libuv` `getaddrinfo(3)` interface.
class Resolver {
public:
  /// Instantiate a resolver based on an event loop.
  explicit Resolver(const Loop &loop) : loop_{&loop} {}

  /// Resolve a host and port string. Throws an `UvcoException` upon error.
  Promise<AddressHandle> gai(std::string_view host, std::string_view port,
                             int af_hint = AF_UNSPEC);
  /// Resolve a host string and numeric port. Throws an `UvcoException` upon
  /// error.
  Promise<AddressHandle> gai(std::string_view host, uint16_t port,
                             int af_hint = AF_UNSPEC);

private:
  const Loop *loop_;

  /// Called after finishing a getaddrinfo call.
  static void onAddrinfo(uv_getaddrinfo_t *req, uv_status status,
                         struct addrinfo *result);
};

/// @}

} // namespace uvco
