// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

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
  AddressHandle(const struct addrinfo *ai);
  AddressHandle(const struct sockaddr *sa);

  std::string address() const { return std::visit(NtopHelper_{}, addr_); }
  uint16_t port() const;
  std::string toString() const;

  int family() const;

  const struct sockaddr *sockaddr() const;

private:
  std::variant<struct sockaddr_in, struct sockaddr_in6> addr_{};

  struct NtopHelper_ {
    std::string operator()(const struct sockaddr_in &ipv4);
    std::string operator()(const struct sockaddr_in6 &ipv6);
    std::string ntop(int family, void *addr);
  };
};

class Resolver {
  struct AddrinfoAwaiter_;

public:
  explicit Resolver(uv_loop_t *loop) : loop_{loop} {}

  Promise<AddressHandle> gai(std::string_view host, std::string_view port,
                             int af_hint = AF_UNSPEC);

private:
  uv_loop_t *loop_;

  static void onAddrinfo(uv_getaddrinfo_t *req, int status,
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

} // namespace uvco
