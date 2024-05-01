
// WIP! A very ugly, early version of Curl support. Basic HTTP/HTTPS downloads
// work, but there are many issues, not the least in code quality.

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
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <uv.h>
#include <vector>

namespace uvco {

/// Contains references to the libcurl multi handle and the libuv loop.
/// Only valid as long as one loop instance is running. Used as context for
/// curl socket functions (SOCKETDATA) and timer functions (TIMERDATA).
class UvCurlContext_ {
public:
  explicit UvCurlContext_(const Loop &loop);
  UvCurlContext_(const UvCurlContext_ &) = delete;
  UvCurlContext_(UvCurlContext_ &&) = delete;
  UvCurlContext_ &operator=(const UvCurlContext_ &) = delete;
  UvCurlContext_ &operator=(UvCurlContext_ &&) = delete;
  ~UvCurlContext_() {
    uv_timer_stop(&timer_);
    curl_multi_cleanup(multi_);
  }

  void checkCurlInfo() const;
  // Curl callbacks:

  /// Called by Curl to inform us of a new socket to monitor or a socket to be
  /// removed.
  static int curlSocketFunction(CURL *easy, curl_socket_t socket, int action,
                                void *userp, void * /* socketp */);

  /// Called by Curl to inform us of a timeout to set up.
  static int curlTimerFunction(CURLM * /* multi */, long timeoutMs,
                               void *userp);

  // libuv callbacks:

  /// Called by libuv when a timeout timer expires.
  static void onUvTimeout(uv_timer_t *timer);

  /// Called by libuv when there is activity on a curl socket.
  static void onCurlSocketActive(uv_poll_t *poll, int status, int events);

  CURLM *multi_;
  uv_loop_t *loop_;
  uv_timer_t timer_;
};

/// Used as awaiter by Curl::download.
class CurlRequest_ {
  static void onCurlDataAvailable(char *data, size_t size, size_t nmemb,
                                  void *userp);

public:
  CurlRequest_(const CurlRequest_ &) = delete;
  CurlRequest_(CurlRequest_ &&) = delete;
  CurlRequest_ &operator=(const CurlRequest_ &) = delete;
  CurlRequest_ &operator=(CurlRequest_ &&) = delete;

  /// Construct and initialize download.
  CurlRequest_(UvCurlContext_ &ctx, std::string url)
      : context_{ctx}, easyHandle_(curl_easy_init()), url_{std::move(url)} {
    // Configure easy handle and initiate download.
    curl_easy_setopt(easyHandle_, CURLOPT_URL, url_.data());
    curl_easy_setopt(easyHandle_, CURLOPT_WRITEFUNCTION, onCurlDataAvailable);
    // Set Write Data for use in onCurlDataAvailable.
    curl_easy_setopt(easyHandle_, CURLOPT_WRITEDATA, this);
    // Set private data for use in curlSocketFunction.
    curl_easy_setopt(easyHandle_, CURLOPT_PRIVATE, this);
    curl_multi_add_handle(context_.multi_, easyHandle_);
  }

  ~CurlRequest_() {
    curl_multi_remove_handle(context_.multi_, easyHandle_);
    curl_easy_cleanup(easyHandle_);
    easyHandle_ = nullptr;
  }

  Promise<void> close() {
    uvStopPoll();
    return closeHandle(&poll_);
  }

  /// Called when data is available.
  void onDataAvailable(char *data, size_t size) {
    chunks_.emplace_back(data, size);
    if (downloaderHandle_) {
      auto handle = downloaderHandle_.value();
      downloaderHandle_.reset();
      Loop::enqueue(handle);
    }
  }

  void onError(uv_status status) noexcept {
    if (downloaderHandle_) {
      auto handle = downloaderHandle_.value();
      lastStatus_ = status;
      downloaderHandle_.reset();
      Loop::enqueue(handle);
    }
  }

  // CurlRequest_ is an awaiter yielding chunks as they are downloaded.

  [[nodiscard]] bool await_ready() const noexcept {
    return !chunks_.empty() || curlStatus_ > 0;
  }

  bool await_suspend(std::coroutine_handle<> handle) noexcept {
    BOOST_ASSERT(!downloaderHandle_);
    downloaderHandle_ = handle;
    return true;
  }

  std::optional<std::string> await_resume() {
    if (lastStatus_ && *lastStatus_ != 0) {
      throw UvcoException{*lastStatus_, "while downloading " + url_};
    }
    if (curlStatus_ && *curlStatus_ != 200) {
      // HTTP Status to be checked by downloader.
      return std::nullopt;
    }
    if (!chunks_.empty()) {
      std::string thisResult = std::move(chunks_.front());
      chunks_.erase(chunks_.begin());
      return std::move(thisResult);
    }

    // Signal end of stream.
    return std::nullopt;
  }

  UvCurlContext_ &context() noexcept { return context_; }
  void initPoll(curl_socket_t newSocket) noexcept {
    if (poll_) {
      return;
    }
    socket_ = newSocket;
    uv_poll_init_socket(context_.loop_, &poll_.emplace(), socket_);
    uv_handle_set_data((uv_handle_t *)(&poll_.value()), this);
  }

  void uvStartPoll(unsigned int events) noexcept {
    BOOST_ASSERT(poll_);
    uv_poll_start(&poll_.value(), static_cast<int>(events),
                  UvCurlContext_::onCurlSocketActive);
  }
  void uvStopPoll() noexcept {
    if (poll_) {
      uv_poll_stop(&poll_.value());
    }
  }

  [[nodiscard]] curl_socket_t socket() const noexcept { return socket_; }
  void setCurlStatus(int status) noexcept {
    if (!curlStatus_) {
      curlStatus_ = status;
    }
  }
  [[nodiscard]] int curlStatus() const noexcept {
    return curlStatus_.value_or(0);
  }

private:
  UvCurlContext_ &context_;
  CURL *easyHandle_;
  curl_socket_t socket_ = -1;

  std::string url_;
  std::optional<std::coroutine_handle<>> downloaderHandle_;
  std::optional<uv_poll_t> poll_;

  // Misuse vector as deque for now.
  std::vector<std::string> chunks_;
  std::optional<uv_status> lastStatus_;
  std::optional<int> curlStatus_;
};

void CurlRequest_::onCurlDataAvailable(char *data, size_t size, size_t nmemb,
                                       void *userp) {
  auto *request = static_cast<CurlRequest_ *>(userp);
  request->onDataAvailable(data, size * nmemb);
}

UvCurlContext_::UvCurlContext_(const Loop &loop)
    : multi_{curl_multi_init()}, loop_{loop.uvloop()}, timer_{} {
  uv_timer_init(loop_, &timer_);
  // Set handle data for onUvTimeout callback.
  uv_handle_set_data((uv_handle_t *)(&timer_), this);

  // Initialize Curl callbacks.
  curl_multi_setopt(multi_, CURLMOPT_SOCKETFUNCTION,
                    UvCurlContext_::curlSocketFunction);
  curl_multi_setopt(multi_, CURLMOPT_SOCKETDATA, this);
  curl_multi_setopt(multi_, CURLMOPT_TIMERFUNCTION,
                    UvCurlContext_::curlTimerFunction);
  curl_multi_setopt(multi_, CURLMOPT_TIMERDATA, this);
}

void UvCurlContext_::checkCurlInfo() const {
  CURLMsg *msg{};
  int curlStatus{};
  int inQueue{};

  // Check for completed requests.
  while (nullptr != (msg = curl_multi_info_read(multi_, &inQueue))) {
    if (msg->msg == CURLMSG_DONE) {
      CurlRequest_ *request{};
      curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &request);
      curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &curlStatus);
      request->setCurlStatus(curlStatus);
      request->onError(0);
    }
  }
}

int UvCurlContext_::curlTimerFunction(CURLM * /* multi */, long timeoutMs,
                                      void *userp) {
  auto *curl = static_cast<UvCurlContext_ *>(userp);
  uv_timer_t &timer = curl->timer_;
  if (timeoutMs < 0) {
    uv_timer_stop(&timer);
  } else {
    uv_timer_start(&timer, UvCurlContext_::onUvTimeout, timeoutMs, 0);
  }
  return 0;
}

void UvCurlContext_::onUvTimeout(uv_timer_t *timer) {
  auto *curl =
      static_cast<UvCurlContext_ *>(uv_handle_get_data((uv_handle_t *)timer));
  int runningHandles = 0;
  curl_multi_socket_action(curl->multi_, CURL_SOCKET_TIMEOUT, 0,
                           &runningHandles);
}

void UvCurlContext_::onCurlSocketActive(uv_poll_t *poll, int status,
                                        int events) {
  int runningHandles{};
  unsigned int flags{};
  auto *request =
      static_cast<CurlRequest_ *>(uv_handle_get_data((uv_handle_t *)poll));

  if (status != 0) {
    request->onError(status);
    return;
  }

  if ((static_cast<unsigned>(events) & UV_READABLE) != 0) {
    flags |= CURL_CSELECT_IN;
  }
  if ((static_cast<unsigned>(events) & UV_WRITABLE) != 0) {
    flags |= CURL_CSELECT_OUT;
  }

  curl_multi_socket_action(request->context().multi_, request->socket(),
                           static_cast<int>(flags), &runningHandles);
  request->context().checkCurlInfo();
}

int UvCurlContext_::curlSocketFunction(CURL *easy, curl_socket_t socket,
                                       int action, void *userp,
                                       void * /* socketp */) {
  auto *curl = static_cast<UvCurlContext_ *>(userp);
  CurlRequest_ *request;
  curl_easy_getinfo(easy, CURLINFO_PRIVATE, &request);

  switch (action) {
  case CURL_POLL_IN:
  case CURL_POLL_OUT:
  case CURL_POLL_INOUT: {
    unsigned int watchFor{};

    if (action != CURL_POLL_IN) {
      watchFor |= UV_WRITABLE;
    }
    if (action != CURL_POLL_OUT) {
      watchFor |= UV_READABLE;
    }

    request->initPoll(socket);
    request->uvStartPoll(watchFor);
    break;
  }

  case CURL_POLL_REMOVE: {
    request->uvStopPoll();
    break;
  }

  default:
    BOOST_ASSERT(false);
  }

  curl->checkCurlInfo();

  return 0;
}

Curl::Curl(const Loop &loop)
    : context_{std::make_unique<UvCurlContext_>(loop)} {}

Curl::~Curl() = default;

Promise<void> Curl::close() { return closeHandle(&context_->timer_); }

MultiPromise<std::string> Curl::download(std::string url) {
  // Sets up download.
  CurlRequest_ request{*context_, std::move(url)};

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

  if (request.curlStatus() != 200) {
    throw UvcoException{UV_EINVAL,
                        fmt::format("HTTP Error {}", request.curlStatus())};
  }
  co_return;
}

} // namespace uvco
