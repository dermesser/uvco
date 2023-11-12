#pragma once

#include <span>
#include <uv.h>

#include "name_resolution.h"
#include "promise.h"

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
  ~Udp() { assert(!udp_); }

  Promise<void> bind(std::string_view address, uint16_t port,
                     unsigned int flag = 0);

  Promise<void> connect(std::string_view address, uint16_t port,
                        bool ipv6only = false);

  template <typename T>
  Promise<void> send(std::span<T> buffer, std::optional<AddressHandle> ah) {
    SendAwaiter_ sendAwaiter{};
    uv_udp_send_t req;
    req.data = &sendAwaiter;

    std::array<uv_buf_t, 1> bufs{};
    bufs[0].base = &(*buffer.begin());
    bufs[0].len = buffer.size_bytes();

    struct sockaddr *addr = nullptr;
    if (ah)
      addr = ah->sockaddr();

    int status =
        uv_udp_send(&req, udp_.get(), bufs.begin(), 1, addr, onSendDone);
    if (status != 0)
      throw UvcoException{status, "uv_udp_send() failed immediately"};

    int status_done = co_await sendAwaiter;
    if (status_done != 0)
      throw UvcoException{status_done, "uv_udp_send() failed while sending"};

    co_return;
  }

  Promise<std::string> receiveOne();

  Promise<std::pair<std::string, AddressHandle>> receiveOneFrom();

  MultiPromise<std::pair<std::string, AddressHandle>> receiveMany();

  Promise<void> close();

private:
  uv_loop_t *loop_;
  std::unique_ptr<uv_udp_t> udp_;
  bool connected_ = false;

  static void onReceiveOne(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                           const struct sockaddr *addr, unsigned int flags);

  struct RecvAwaiter_ {
    [[nodiscard]] bool await_ready() const;
    bool await_suspend(std::coroutine_handle<> h);
    std::string await_resume();

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
