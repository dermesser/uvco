
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
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <uv.h>
#include <vector>

namespace uvco {

class CurlRequest_;

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

  /// Add a Curl easy handle to the multi handle.
  void addHandle(CURL *handle) { curl_multi_add_handle(multi_, handle); }

  /// Remove a Curl easy handle from the multi handle.
  void removeHandle(CURL *handle) { curl_multi_remove_handle(multi_, handle); }

  /// Checks with Curl for any completed/errored requests.
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

  // I/O: tying together libuv and curl.

  void initPoll(CurlRequest_ *request, curl_socket_t newSocket) noexcept {
    if (polls_.contains(newSocket)) {
      return;
    }
    const auto newPollCtx = polls_.emplace(newSocket, uv_poll_t{});
    uv_poll_t &poll = newPollCtx.first->second.poll;
    newPollCtx.first->second.request = request;
    uv_poll_init_socket(loop_, &poll, newSocket);
    uv_handle_set_data((uv_handle_t *)&poll, this);
  }

  void uvStartPoll(curl_socket_t socket, unsigned int events) noexcept {
    const auto it = polls_.find(socket);
    BOOST_ASSERT(it != polls_.end());
    uv_poll_t &poll = it->second.poll;
    uv_poll_start(&poll, static_cast<int>(events),
                  UvCurlContext_::onCurlSocketActive);
  }

  void uvStopPoll(curl_socket_t socket) noexcept {
    const auto it = polls_.find(socket);
    if (it != polls_.end()) {
      uv_poll_stop(&it->second.poll);
    }
  }

  /// Returns the CurlRequest associated with the given socket. This is
  /// essential for notifying individual requests of socket errors; socket
  /// errors occur in a uv callbcak which only has access to the socket.
  CurlRequest_ &getRequest(curl_socket_t socket) {
    const auto it = polls_.find(socket);
    BOOST_ASSERT(it != polls_.end());
    return *it->second.request;
  }

  /// Close all open sockets and the timer.
  Promise<void> close() {
    co_await closeHandle(&timer_);

    std::vector<Promise<void>> promises;
    promises.reserve(polls_.size());
    for (auto &[socket, poll] : polls_) {
      promises.push_back(closeHandle(&poll));
    }
    for (auto &promise : promises) {
      co_await promise;
    }
  }

private:
  /// A uv poll handle with its associated CurlRequest_.
  struct PollCtx {
    uv_poll_t poll;
    CurlRequest_ *request;
  };

  CURLM *multi_;
  uv_loop_t *loop_;
  uv_timer_t timer_;
  std::map<curl_socket_t, PollCtx> polls_;
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
    curl_easy_setopt(easyHandle_, CURLOPT_FOLLOWLOCATION, 1L);
    context_.addHandle(easyHandle_);
  }

  ~CurlRequest_() {
    context_.removeHandle(easyHandle_);
    curl_easy_cleanup(easyHandle_);
    easyHandle_ = nullptr;
  }

  /// Get the URL of the request.
  [[nodiscard]] std::string_view url() const noexcept { return url_; }

  /// Called when data is available. Resumes the downloader coroutine.
  void onDataAvailable(char *data, size_t size) {
    BOOST_ASSERT(data != nullptr);
    chunks_.emplace_back(data, size);
    if (downloaderHandle_) {
      auto handle = downloaderHandle_.value();
      downloaderHandle_.reset();
      Loop::enqueue(handle);
    }
  }

  /// Called when an error occurs, but also upon completion (then status == 0).
  ///
  /// Resumes the downloader coroutine.
  void onError(uv_status status) noexcept {
    if (downloaderHandle_) {
      auto handle = downloaderHandle_.value();
      setUvStatus(status);
      downloaderHandle_.reset();
      Loop::enqueue(handle);
    }
  }

  // CurlRequest_ is an awaiter yielding chunks as they are downloaded.

  /// Awaiter protocol: ready if there are chunks to yield or the download is
  /// done.
  [[nodiscard]] bool await_ready() const noexcept {
    return !chunks_.empty() || responseCode_ > 0;
  }

  /// Awaiter protocol: suspend the downloader coroutine.
  bool await_suspend(std::coroutine_handle<> handle) noexcept {
    BOOST_ASSERT(!downloaderHandle_);
    downloaderHandle_ = handle;
    return true;
  }

  /// Awaiter protocol: yield the next chunk or signal end of stream.
  std::optional<std::string> await_resume() {
    if (uvStatus_ && *uvStatus_ != 0) {
      return std::nullopt;
    }
    if (verifyResult_ && *verifyResult_ != 0) {
      // SSL verification to be checked by downloader.
      return std::nullopt;
    }
    if (responseCode_ && *responseCode_ != 200) {
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

  // Error codes.

  /// Set socket error status.
  void setUvStatus(uv_status status) noexcept {
    if (!uvStatus_) {
      uvStatus_ = status;
    }
  }

  /// Set HTTP response code.
  void setResponseCode(int status) noexcept {
    if (!responseCode_) {
      responseCode_ = status;
    }
  }

  /// Set SSL verification result.
  void setVerifyResult(int result) noexcept {
    if (!verifyResult_) {
      verifyResult_ = result;
    }
  }

  /// Get uv error status. 0 if success.
  [[nodiscard]] uv_status uvStatus() const noexcept {
    return uvStatus_.value_or(0);
  }

  /// Get HTTP response code. 0 if not set.
  [[nodiscard]] int responseCode() const noexcept {
    return responseCode_.value_or(0);
  }

  /// Get SSL verification result. 0 means success, > 0 means failure.
  [[nodiscard]] int verifyResult() const noexcept {
    return verifyResult_.value_or(0);
  }

private:
  UvCurlContext_ &context_;
  CURL *easyHandle_;

  std::string url_;
  std::optional<std::coroutine_handle<>> downloaderHandle_;

  // Misuse vector as deque for now.
  std::vector<std::string> chunks_;
  std::optional<uv_status> uvStatus_;
  std::optional<int> responseCode_;
  std::optional<int> verifyResult_;
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
  int responseCode{};
  int verifyResult{};
  int inQueue{};

  // Check for completed requests.
  while (nullptr != (msg = curl_multi_info_read(multi_, &inQueue))) {
    if (msg->msg == CURLMSG_DONE) {
      CurlRequest_ *request{};
      curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &request);
      curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE,
                        &responseCode);
      curl_easy_getinfo(msg->easy_handle, CURLINFO_SSL_VERIFYRESULT,
                        &verifyResult);
      request->setResponseCode(responseCode);
      request->setVerifyResult(verifyResult);
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
  auto *context =
      static_cast<UvCurlContext_ *>(uv_handle_get_data((uv_handle_t *)poll));

  curl_socket_t socket{};
  const uv_status filenoStatus = uv_fileno((uv_handle_t *)poll, &socket);
  if (filenoStatus != 0) {
    throw UvcoException{filenoStatus, "while getting file descriptor"};
  }

  if (status != 0) {
    context->getRequest(socket).onError(status);
    return;
  }

  if ((static_cast<unsigned>(events) & UV_READABLE) != 0) {
    flags |= CURL_CSELECT_IN;
  }
  if ((static_cast<unsigned>(events) & UV_WRITABLE) != 0) {
    flags |= CURL_CSELECT_OUT;
  }

  curl_multi_socket_action(context->multi_, socket, static_cast<int>(flags),
                           &runningHandles);
  context->checkCurlInfo();
}

int UvCurlContext_::curlSocketFunction(CURL *easy, curl_socket_t socket,
                                       int action, void *userp,
                                       void * /* socketp */) {
  auto *context = static_cast<UvCurlContext_ *>(userp);

  switch (action) {
  case CURL_POLL_IN:
  case CURL_POLL_OUT:
  case CURL_POLL_INOUT: {
    unsigned int watchFor{};

    // Obtain request object so it can be associated with socket for error
    // handling.
    CurlRequest_ *request{};
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &request);

    if (action != CURL_POLL_IN) {
      watchFor |= UV_WRITABLE;
    }
    if (action != CURL_POLL_OUT) {
      watchFor |= UV_READABLE;
    }

    context->initPoll(request, socket);
    context->uvStartPoll(socket, watchFor);
    break;
  }

  case CURL_POLL_REMOVE: {
    context->uvStopPoll(socket);
    break;
  }

  default:
    BOOST_ASSERT(false);
  }

  context->checkCurlInfo();

  return 0;
}

Curl::Curl(const Loop &loop)
    : context_{std::make_unique<UvCurlContext_>(loop)} {}

Curl::~Curl() = default;

Promise<void> Curl::close() { co_await context_->close(); }

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

  if (request.uvStatus() != 0) {
    throw UvcoException{
        request.uvStatus(),
        fmt::format("Socket error while fetching {}", request.url())};
  }
  if (request.verifyResult() != 0) {
    throw UvcoException{UV_EINVAL, fmt::format("SSL verification failed for {}",
                                               request.url())};
  }
  if (request.responseCode() != 200) {
    throw UvcoException{UV_EINVAL,
                        fmt::format("HTTP Error {} while fetching {}",
                                    request.responseCode(), request.url())};
  }

  co_return;
}

} // namespace uvco
