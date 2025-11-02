// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <boost/assert.hpp>
#include <sys/socket.h>
#include <sys/types.h>
#include <uv.h>
#include <uv/unix.h>

#include "uvco/bounded_queue.h"
#include "uvco/internal/internal_utils.h"
#include "uvco/name_resolution.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"

#include <coroutine>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace uvco {

/// @addtogroup UDP
/// @{

/// Interface to UDP functionality: can be connected or disconnected datagram
/// client/server.
class Udp {
public:
  /// Set up a UDP object.
  explicit Udp(const Loop &loop);
  Udp(Udp &&other) = default;
  Udp &operator=(Udp &&other) = default;
  Udp(const Udp &) = delete;
  Udp &operator=(const Udp &) = delete;
  ~Udp();

  /// Bind UDP socket to address.
  Promise<void> bind(std::string_view address, uint16_t port,
                     unsigned int flag = 0);
  Promise<void> bind(const AddressHandle &address, unsigned int flag = 0);

  /// Connect UDP socket to address.
  Promise<void> connect(std::string_view address, uint16_t port,
                        bool ipv6only = false);
  Promise<void> connect(const AddressHandle &address);

  /// Send to address, or send to connected peer. Must be a mutable
  /// buffer because libuv requires it - the buffer will not be modified.
  Promise<void> send(std::span<char> buffer,
                     std::optional<AddressHandle> address = {});

  /// Receive a single UDP packet.
  ///
  /// TODO: use a better-suited buffer type.
  ///
  /// Only one coroutine can be receiving at a time. This is currently enforced
  /// by assertions.
  Promise<std::string> receiveOne();

  /// Receive a single UDP packet and also return the sender's address.
  ///
  /// Only one coroutine can be receiving at a time. This is currently enforced
  /// by assertions.
  Promise<std::pair<std::string, AddressHandle>> receiveOneFrom();

  /// Generate packets received on socket. Call stopReceiveMany() when no more
  /// packets are desired; otherwise this will continue indefinitely.
  ///
  /// Only one coroutine can be receiving at a time. This is currently enforced
  /// by assertions.
  MultiPromise<std::pair<std::string, AddressHandle>> receiveMany();

  /// Stop receiving with `receiveMany()` by cancelling the receiving generator
  /// coroutine. Supply the MultiPromise obtained from receiveMany() in order to
  /// guarantee a complete clean-up.
  void
  stopReceiveMany(MultiPromise<std::pair<std::string, AddressHandle>> &packets);

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

private:
  const Loop *loop_;
  std::unique_ptr<uv_udp_t> udp_;
  bool connected_ = false;

  int udpStartReceive();
  void udpStopReceive();

  static void onReceiveOne(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                           const struct sockaddr *addr, unsigned int flags);

  struct RecvAwaiter_ {
    static constexpr unsigned packetQueueSize = 128;
    using QueueItem_ =
        std::variant<std::pair<std::string, AddressHandle>, uv_status>;

    RecvAwaiter_();
    [[nodiscard]] bool await_ready() const;
    bool await_suspend(std::coroutine_handle<> handle);
    std::optional<std::pair<std::string, AddressHandle>> await_resume();

    BoundedQueue<QueueItem_> buffer_;
    std::optional<std::coroutine_handle<>> handle_;
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
