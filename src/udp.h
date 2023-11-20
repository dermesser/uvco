// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <boost/assert.hpp>
#include <uv.h>

#include "name_resolution.h"
#include "promise.h"

#include <span>

namespace uvco {

class Udp {
public:
  explicit Udp(uv_loop_t *loop)
      : loop_{loop}, udp_{std::make_unique<uv_udp_t>()} {
    uv_udp_init(loop, udp_.get());
  }
  Udp(Udp &&other) = default;
  Udp &operator=(Udp &&other) = default;
  Udp(const Udp &) = delete;
  Udp &operator=(const Udp &) = delete;
  ~Udp() { BOOST_ASSERT(!udp_); }

  Promise<void> bind(std::string_view address, uint16_t port,
                     unsigned int flag = 0);

  Promise<void> connect(std::string_view address, uint16_t port,
                        bool ipv6only = false);

  Promise<void> send(std::span<char> buffer,
                     std::optional<AddressHandle> address);

  Promise<std::string> receiveOne();

  Promise<std::pair<std::string, AddressHandle>> receiveOneFrom();

  // Generate packets received on socket. Call stopReceiveMany() when no more
  // packets are desired; otherwise this will continue indefinitely.
  MultiPromise<std::pair<std::string, AddressHandle>> receiveMany();
  // Stop receiving after the next packet.
  void stopReceiveMany();

  Promise<void> close();

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

  static void onSendDone(uv_udp_send_t *req, int status);

  struct SendAwaiter_ {
    [[nodiscard]] bool await_ready() const;
    bool await_suspend(std::coroutine_handle<> h);
    int await_resume();

    std::optional<std::coroutine_handle<>> handle_;
    std::optional<int> status_;
  };
};

} // namespace uvco
