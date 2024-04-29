
// WIP! A very ugly, early version of Curl support. Basic HTTP/HTTPS downloads
// work, but there are many issues, not the least in code quality.

#include <algorithm>
#include <boost/assert.hpp>
#include <cstdio>
#include <cstring>
#include <curl/curl.h>
#include <curl/easy.h>
#include <curl/multi.h>

#include "close.h"
#include "exception.h"
#include "integrations/curl/curl.h"
#include "internal/internal_utils.h"
#include "loop/loop.h"
#include "promise/multipromise.h"
#include "promise/promise.h"

#include <coroutine>
#include <cstddef>
#include <fmt/core.h>
#include <optional>
#include <string>
#include <utility>
#include <uv.h>
#include <vector>

namespace uvco {

namespace {

void onCurlDataAvailable(char *data, size_t size, size_t nmemb, void *userp);

/// Used as awaiter by Curl::download.
class CurlRequest_ {
public:
  CurlRequest_(const CurlRequest_ &) = delete;
  CurlRequest_(CurlRequest_ &&) = delete;
  CurlRequest_ &operator=(const CurlRequest_ &) = delete;
  CurlRequest_ &operator=(CurlRequest_ &&) = delete;

  /// Construct and initialize download.
  CurlRequest_(CURLM *multiHandle, std::string url)
      : multiHandle_{multiHandle}, easyHandle_(curl_easy_init()),
        url_{std::move(url)} {
    curl_easy_setopt(easyHandle_, CURLOPT_URL, url_.data());
    curl_easy_setopt(easyHandle_, CURLOPT_WRITEFUNCTION, onCurlDataAvailable);
    curl_easy_setopt(easyHandle_, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(easyHandle_, CURLOPT_PRIVATE, this);
    curl_multi_add_handle(multiHandle, easyHandle_);
  }

  ~CurlRequest_() {
    curl_multi_remove_handle(multiHandle_, easyHandle_);
    curl_easy_cleanup(easyHandle_);
    multiHandle_ = nullptr;
    easyHandle_ = nullptr;
  }

  Promise<void> close() { return closeHandle(&poll_); }

  /// Called when data is available.
  void onDataAvailable(char *data, size_t size) {
    chunks_.emplace_back(data, size);
    if (handle_) {
      auto handle = handle_.value();
      handle_.reset();
      Loop::enqueue(handle);
    }
  }

  void onError(uv_status status) noexcept {
    if (handle_) {
      auto handle = handle_.value();
      lastStatus_ = status;
      handle_.reset();
      Loop::enqueue(handle);
    }
  }

  void checkCurlInfo() {
    CURLMsg *msg;

    while (nullptr !=
           (msg = curl_multi_info_read(multiHandle_, &curlStatus_))) {
      if (msg->msg == CURLMSG_DONE) {
        curl_easy_getinfo(easyHandle_, CURLINFO_RESPONSE_CODE, &curlStatus_);
        onError(0);
        break;
      }
    }
  }

  // CurlRequest is an awaiter.

  [[nodiscard]] bool await_ready() const noexcept {
    return !chunks_.empty() || curlStatus_ > 0;
  }

  bool await_suspend(std::coroutine_handle<> handle) noexcept {
    BOOST_ASSERT(!handle_);
    handle_ = handle;
    return true;
  }

  std::optional<std::string> await_resume() {
    if (!chunks_.empty()) {
      std::string thisResult = std::move(chunks_.front());
      chunks_.erase(chunks_.begin());
      return std::move(thisResult);
    }
    if (lastStatus_ != 0) {
      throw UvcoException{lastStatus_, "while downloading " + url_};
    }

    // Signal end of stream.
    return std::nullopt;
  }

  uv_poll_t &uvPoll() noexcept { return poll_; }
  curl_socket_t &socket() noexcept { return socket_; }
  CURLM *multiHandle() noexcept { return multiHandle_; }
  void setCurlStatus(int status) noexcept { curlStatus_ = status; }

private:
  CURLM *multiHandle_;
  CURL *easyHandle_;
  curl_socket_t socket_ = -1;

  std::string url_;
  std::optional<std::coroutine_handle<>> handle_;
  uv_poll_t poll_{};

  // Misuse vector as deque for now.
  std::vector<std::string> chunks_;
  uv_status lastStatus_ = {};
  // > 0 once finished.
  int curlStatus_ = {};
};

// UV callbacks

void onUvTimeout(uv_timer_t *timer) {
  auto *curl = static_cast<Curl *>(uv_handle_get_data((uv_handle_t *)timer));
  int runningHandles = 0;
  curl_multi_socket_action(curl->multi(), CURL_SOCKET_TIMEOUT, 0,
                           &runningHandles);
}

void onCurlSocketActive(uv_poll_t *poll, int status, int events) {
  int runningHandles = 0;
  unsigned int flags = 0;
  auto *request =
      static_cast<CurlRequest_ *>(uv_handle_get_data((uv_handle_t *)poll));

  if (status != 0) {
    request->onError(status);
    return;
  }

  if (events & UV_READABLE) {
    flags |= CURL_CSELECT_IN;
  }
  if (events & UV_WRITABLE) {
    flags |= CURL_CSELECT_OUT;
  }

  curl_multi_socket_action(request->multiHandle(), request->socket(), flags,
                           &runningHandles);
  request->checkCurlInfo();
}

// Curl callbacks

void onCurlDataAvailable(char *data, size_t size, size_t nmemb, void *userp) {
  auto *request = static_cast<CurlRequest_ *>(userp);
  request->onDataAvailable(data, size * nmemb);
}

int curlSocketFunction(CURL *easy, curl_socket_t socket, int action,
                       void *userp, void *socketp) {
  auto *curl = static_cast<Curl *>(userp);
  CurlRequest_ *request;
  curl_easy_getinfo(easy, CURLINFO_PRIVATE, &request);

  switch (action) {
  case CURL_POLL_IN:
  case CURL_POLL_OUT:
  case CURL_POLL_INOUT: {
    int watchFor = 0;

    if (action != CURL_POLL_IN) {
      watchFor |= UV_WRITABLE;
    }
    if (action != CURL_POLL_OUT) {
      watchFor |= UV_READABLE;
    }

    if (request->socket() < 0) {
      // Only create poll handle once. This doesn't work if Curl opens more
      // than one socket per CurlRequest_.
      request->socket() = socket;
      uv_poll_init(curl->loop(), &request->uvPoll(), socket);
    }
    uv_handle_set_data(reinterpret_cast<uv_handle_t *>(&request->uvPoll()),
                       request);
    uv_poll_start(&request->uvPoll(), watchFor, onCurlSocketActive);
    break;
  }

  case CURL_POLL_REMOVE: {
    uv_poll_stop(&request->uvPoll());
    curl_multi_assign(curl->multi(), socket, nullptr);
    break;
  }

  default:
    BOOST_ASSERT(false);
  }

  request->checkCurlInfo();

  return 0;
}

int curlTimerFunction(CURLM *multi, long timeoutMs, void *userp) {
  auto *curl = static_cast<Curl *>(userp);
  uv_timer_t &timer = curl->timer();
  if (timeoutMs < 0) {
    uv_timer_stop(&timer);
  } else {
    timeoutMs = std::max(timeoutMs, 1L);
    uv_timer_start(&timer, onUvTimeout, timeoutMs, 0);
  }
  return 0;
}

} // namespace

Curl &Curl::operator=(Curl &&other) noexcept {
  if (this != &other) {
    multi_ = other.multi_;
    loop_ = other.loop_;
    other.multi_ = nullptr;
    other.loop_ = nullptr;
    // Let's hope this works as intended...
    curl_multi_setopt(multi_, CURLMOPT_SOCKETDATA, this);
  }
  return *this;
}

Curl::Curl(Curl &&other) noexcept {
  if (this != &other) {
    multi_ = other.multi_;
    loop_ = other.loop_;
    other.multi_ = nullptr;
    other.loop_ = nullptr;
    curl_multi_setopt(multi_, CURLMOPT_SOCKETDATA, this);
  }
}

Curl::Curl(const Loop &loop) : multi_{curl_multi_init()}, loop_{loop.uvloop()} {
  uv_timer_init(loop_, &timer_);
  uv_handle_set_data(reinterpret_cast<uv_handle_t *>(&timer_), this);
  curl_multi_setopt(multi_, CURLMOPT_SOCKETFUNCTION, curlSocketFunction);
  curl_multi_setopt(multi_, CURLMOPT_SOCKETDATA, this);
  curl_multi_setopt(multi_, CURLMOPT_TIMERFUNCTION, curlTimerFunction);
  curl_multi_setopt(multi_, CURLMOPT_TIMERDATA, this);
}

Promise<void> Curl::close() { return closeHandle(&timer_); }

Curl::~Curl() { curl_multi_cleanup(multi_); }

MultiPromise<std::string> Curl::download(std::string url) {
  CurlRequest_ request{multi_, std::move(url)};

  while (true) {
    auto result = co_await request;
    if (result.has_value()) {
      co_yield std::move(result.value());
      continue;
    }
    // End of stream.
    break;
  }
  co_await request.close();
  co_return;
}

} // namespace uvco
