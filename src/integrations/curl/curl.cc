// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include <boost/assert.hpp>
#include <cctype>
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
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <uv.h>
#include <vector>

namespace uvco {

namespace {

std::string urlEncode(CURL *curl, std::string_view url) {
  char *result = curl_easy_escape(curl, url.data(), url.size());
  const std::string encoded{result};
  curl_free(result);
  return encoded;
}

/// Format a list of fields for POST requests.
std::string
formattedFields(CURL *curl,
                std::span<const std::pair<std::string, std::string>> fields) {
  std::string result;
  result.reserve(std::accumulate(
      fields.begin(), fields.end(), 0, [](size_t acc, const auto &pair) {
        return acc + pair.first.size() + pair.second.size() + 2;
      }));

  // TODO: URL encoding
  for (const auto &[key, value] : fields) {
    result += urlEncode(curl, key);
    result += '=';
    result += urlEncode(curl, value);
    result += '&';
  }
  return result;
}

} // namespace

class CurlRequestCore_;

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

  void initPoll(CurlRequestCore_ *request, curl_socket_t newSocket) noexcept {
    const auto it = polls_.find(newSocket);
    if (it != polls_.end()) {
      // Already initialized. Ensure that socket is associated with the correct
      // request in case Curl reuses a socket.
      it->second.request = request;
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
  CurlRequestCore_ &getRequest(curl_socket_t socket) {
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
    polls_.clear();
  }

private:
  /// A uv poll handle with its associated CurlRequest_.
  struct PollCtx {
    uv_poll_t poll;
    CurlRequestCore_ *request;
  };

  CURLM *multi_;
  uv_loop_t *loop_;
  uv_timer_t timer_;
  std::map<curl_socket_t, PollCtx> polls_;
};

/// Used as awaiter by Curl::download.
class CurlRequestCore_ {
  static void onCurlDataAvailable(char *data, size_t size, size_t nmemb,
                                  void *userp);

public:
  CurlRequestCore_(const CurlRequestCore_ &) = delete;
  CurlRequestCore_(CurlRequestCore_ &&) = delete;
  CurlRequestCore_ &operator=(const CurlRequestCore_ &) = delete;
  CurlRequestCore_ &operator=(CurlRequestCore_ &&) = delete;

  explicit CurlRequestCore_(std::weak_ptr<UvCurlContext_> ctx)
      : context_{std::move(ctx)}, easyHandle_{curl_easy_init()} {}

  /// Construct and initialize download.
  CurlRequestCore_(std::weak_ptr<UvCurlContext_> ctx, std::string url)
      : context_{std::move(ctx)}, easyHandle_(curl_easy_init()) {
    initGet(std::move(url));
  }

  ~CurlRequestCore_() {
    curl_easy_cleanup(easyHandle_);
    if (!context_.expired()) {
      context_.lock()->removeHandle(easyHandle_);
    }
    easyHandle_ = nullptr;
  }

  void initCommon(std::string url) {
    BOOST_ASSERT(easyHandle_ != nullptr);
    BOOST_ASSERT_MSG(url_.empty(), "cannot work with empty URL");
    BOOST_ASSERT_MSG(!context_.expired(),
                     "Curl object must outlive any request");

    url_ = std::move(url);
    // Configure easy handle and initiate download.
    curl_easy_setopt(easyHandle_, CURLOPT_URL, url_.data());
    curl_easy_setopt(easyHandle_, CURLOPT_WRITEFUNCTION, onCurlDataAvailable);
    // Set Write Data for use in onCurlDataAvailable.
    curl_easy_setopt(easyHandle_, CURLOPT_WRITEDATA, this);
    // Set private data for use in curlSocketFunction.
    curl_easy_setopt(easyHandle_, CURLOPT_PRIVATE, this);
    curl_easy_setopt(easyHandle_, CURLOPT_FOLLOWLOCATION, 1L);
  }

  void initGet(std::string url) {
    initCommon(std::move(url));
    context_.lock()->addHandle(easyHandle_);
  }

  void initPost(std::string url,
                std::span<const std::pair<std::string, std::string>> fields) {
    initCommon(std::move(url));
    BOOST_ASSERT(!payload_);

    // Must keep payload alive until request is done.
    payload_ = formattedFields(easyHandle_, fields);
    curl_easy_setopt(easyHandle_, CURLOPT_POST, 1L);
    curl_easy_setopt(easyHandle_, CURLOPT_POSTFIELDS, payload_->c_str());
    context_.lock()->addHandle(easyHandle_);
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
  friend class CurlRequest;

  std::weak_ptr<UvCurlContext_> context_;
  CURL *easyHandle_;

  std::string url_;
  std::optional<std::string> payload_;
  std::optional<std::coroutine_handle<>> downloaderHandle_;

  // Misuse vector as deque for now.
  std::vector<std::string> chunks_;
  std::optional<uv_status> uvStatus_;
  std::optional<int> responseCode_;
  std::optional<int> verifyResult_;
};

void CurlRequestCore_::onCurlDataAvailable(char *data, size_t size,
                                           size_t nmemb, void *userp) {
  auto *request = static_cast<CurlRequestCore_ *>(userp);
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
  long responseCode{};
  long verifyResult{};
  int inQueue{};

  // Check for completed requests.
  while (nullptr != (msg = curl_multi_info_read(multi_, &inQueue))) {
    if (msg->msg == CURLMSG_DONE) {
      CurlRequestCore_ *request{};
      BOOST_VERIFY(CURLE_OK == curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &request));
      BOOST_VERIFY(CURLE_OK == curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE,
                        &responseCode));
      BOOST_VERIFY(CURLE_OK == curl_easy_getinfo(msg->easy_handle, CURLINFO_SSL_VERIFYRESULT,
                        &verifyResult));
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
    CurlRequestCore_ *request{};
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

CurlRequest Curl::get(std::string url) {
  return CurlRequest{context_, CurlRequest::Method::GET, std::move(url)};
}

CurlRequest
Curl::post(std::string url,
           std::span<const std::pair<std::string, std::string>> fields) {
  return CurlRequest{context_, CurlRequest::Method::POST, std::move(url),
                     fields};
}

Promise<void> Curl::close() { co_await context_->close(); }

CurlRequest::~CurlRequest() = default;

CurlRequest::CurlRequest(std::weak_ptr<UvCurlContext_> context, Method method,
                         std::string url)
    : core_{std::make_unique<CurlRequestCore_>(std::move(context))} {
  switch (method) {
  case Method::GET:
    core_->initGet(std::move(url));
    break;
  default:
    BOOST_ASSERT(false);
  }
}

CurlRequest::CurlRequest(
    std::weak_ptr<UvCurlContext_> context, Method method, std::string url,
    std::span<const std::pair<std::string, std::string>> fields)
    : core_{std::make_unique<CurlRequestCore_>(std::move(context))} {

  switch (method) {
  case Method::POST:
    core_->initPost(std::move(url), fields);
    break;
  default:
    BOOST_ASSERT(false);
  }
}

int CurlRequest::statusCode() const { return core_->responseCode(); }

MultiPromise<std::string> CurlRequest::start() {
  // Run transfer of response.

  while (true) {
    auto result = co_await *core_;
    if (result.has_value()) {
      co_yield std::move(result.value());
      continue;
    }
    // End of stream.
    break;
  }

  if (core_->uvStatus() != 0) {
    throw UvcoException{
        core_->uvStatus(),
        fmt::format("Socket error while fetching {}", core_->url())};
  }
  if (core_->verifyResult() != 0) {
    throw UvcoException{
        UV_EINVAL, fmt::format("SSL verification failed for {}", core_->url())};
  }
  if (core_->responseCode() != 200) {
    throw UvcoException{UV_EINVAL,
                        fmt::format("HTTP Error {} while fetching {}",
                                    core_->responseCode(), core_->url())};
  }

  co_return;
}
} // namespace uvco
