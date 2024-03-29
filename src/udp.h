// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <stdio.h>
#include <sys/socket.h>
#include <uv.h>

#include <boost/assert.hpp>

#include "internal/internal_utils.h"
#include "name_resolution.h"
#include "promise/multipromise.h"
#include "promise/promise.h"
#include "run.h"

#include <coroutine>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <uv/unix.h>

namespace uvco {

/// @addtogroup UDP
/// @{

/// Interface to UDP functionality: can be connected or disconnected datagram
/// client/server.
class Udp {
public:
  /// Set up a UDP object.
  explicit Udp(const Loop &loop)
      : loop_{loop}, udp_{std::make_unique<uv_udp_t>()} {
    uv_udp_init(loop.uvloop(), udp_.get());
  }
  Udp(Udp &&other) = default;
  Udp &operator=(Udp &&other) = default;
  Udp(const Udp &) = delete;
  Udp &operator=(const Udp &) = delete;
  ~Udp();

  /// Bind UDP socket to address.
  ///
  /// TODO: provide `AddressHandle` interface.
  Promise<void> bind(std::string_view address, uint16_t port,
                     unsigned int flag = 0);

  /// Connect UDP socket to address.
  ///
  /// TODO: provide `AddressHandle` interface.
  Promise<void> connect(std::string_view address, uint16_t port,
                        bool ipv6only = false);

  /// Send to address, or send to connected peer. Must be a mutable
  /// buffer because libuv requires it - the buffer will not be modified.
  Promise<void> send(std::span<char> buffer,
                     std::optional<AddressHandle> address = {});

  /// Receive a single UDP packet.
  ///
  /// TODO: use a better-suited buffer type.
  Promise<std::string> receiveOne();

  /// Receive a single UDP packet and also return the sender's address.
  Promise<std::pair<std::string, AddressHandle>> receiveOneFrom();

  /// Generate packets received on socket. Call stopReceiveMany() when no more
  /// packets are desired; otherwise this will continue indefinitely.
  MultiPromise<std::pair<std::string, AddressHandle>> receiveMany();
  /// Stop receiving with `receiveMany()` after the next packet.
  void stopReceiveMany();

  /// Enable sending to broadcast addresses.
  void setBroadcast(bool enabled);
  /// Set TTL on sent packets. TTL must be between 1 and 255.
  void setTtl(uint8_t ttl);
  /// Set multicast interface for sending or receiving data.
  void setMulticastInterface(const std::string &interfaceAddress);
  /// If the loop flag is enabled, sent multicast packets will arrive back on
  /// the sending socket.
  void setMulticastLoop(bool enabled);
  /// Join a multicast group.
  void joinMulticast(const std::string &address, const std::string &interface);
  /// Leave multicast group.
  void leaveMulticast(const std::string &address, const std::string &interface);

  /// Obtain locally bound name of socket.
  [[nodiscard]] AddressHandle getSockname() const;

  /// Obtain peer name if connected.
  [[nodiscard]] std::optional<AddressHandle> getPeername() const;

  /// Close UDP socket. Await on the returned promise to ensure that the socket
  /// is fully closed.
  Promise<void> close();

  [[nodiscard]] uv_udp_t *underlying() const;

private:
  uv_loop_t *loop_;
  std::unique_ptr<uv_udp_t> udp_;
  bool connected_ = false;

  bool is_receiving_ = false;

  int udpStartReceive();
  void udpStopReceive();

  static void onReceiveOne(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                           const struct sockaddr *addr, unsigned int flags);

  struct RecvAwaiter_ {
    [[nodiscard]] bool await_ready() const;
    bool await_suspend(std::coroutine_handle<> h);
    std::optional<std::string> await_resume();

    void resume() {
      if (handle_) {
        handle_->resume();
      }
    }

    std::optional<std::string> buffer_;
    std::optional<std::coroutine_handle<>> handle_;
    std::optional<AddressHandle> addr_;
    std::optional<int> nread_;
    bool stop_receiving_ = true;
  };

  static void onSendDone(uv_udp_send_t *req, uv_status status);

  struct SendAwaiter_ {
    [[nodiscard]] bool await_ready() const;
    bool await_suspend(std::coroutine_handle<> h);
    int await_resume();

    std::optional<std::coroutine_handle<>> handle_;
    std::optional<int> status_;
  };
};

/// @}

} // namespace uvco
