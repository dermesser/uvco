// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "promise/multipromise.h"
#include "promise/promise.h"

#include <curl/curl.h>
#include <curl/multi.h>

#include <memory>
#include <span>
#include <string>
#include <utility>

namespace uvco {

class Loop;

class UvCurlContext_;
class CurlRequestCore_;
class Curl;

class CurlRequest {
public:
  enum class Method { GET, POST };

  CurlRequest(const CurlRequest &) = delete;
  CurlRequest(CurlRequest &&) = delete;
  CurlRequest &operator=(const CurlRequest &) = delete;
  CurlRequest &operator=(CurlRequest &&) = delete;
  ~CurlRequest();

  /// Start the request. This method is a generator that yields received chunks
  /// of the remote resource. Make sure to always `co_await` the returned MultiPromise
  /// until receiving a `std::nullopt`.
  ///
  /// The `Curl` instance must not be closed before the request has finished.
  MultiPromise<std::string> start();

  /// Return the status code of the request. Only valid after the request has
  /// finished (i.e. the `start()` generator has returned `std::nullopt`).
  /// Otherwise 0.
  [[nodiscard]] int statusCode() const;

private:
  friend class Curl;
  /// Initialize a GET request.
  CurlRequest(std::weak_ptr<UvCurlContext_> context, Method method,
              std::string url);
  /// Initialize a POST request.
  CurlRequest(std::weak_ptr<UvCurlContext_> context, Method method,
              std::string url,
              std::span<const std::pair<std::string, std::string>> fields);

  // Core must be pinned in memory.
  std::unique_ptr<CurlRequestCore_> core_;
};

/// A simple Curl client that can download files from the internet.
/// Errors are currently handled for HTTP; other protocols have status codes
/// that this class doesn't yet know about.
///
/// The `download()` method is a generator yielding received chunks of the
/// remote resource. Make sure to always `co_await` the download generator until
/// receiving a `std::nullopt`, and call `close()` after you're done with the
/// Curl handle.
///
/// Downloads can be started and progressing concurrently.
class Curl {
public:
  explicit Curl(const Loop &loop);

  // Pinned in memory due to uv fields. Hide behind unique_ptr if moving
  // is required.
  Curl(const Curl &) = delete;
  Curl(Curl &&other) = delete;
  Curl &operator=(const Curl &) = delete;
  Curl &operator=(Curl &&other) = delete;
  ~Curl();

  /// Prepare a GET request.
  CurlRequest get(std::string url);

  /// Prepare a POST request.
  CurlRequest post(std::string url,
                   std::span<const std::pair<std::string, std::string>> fields);

  /// Close the curl handle in order to free all associated resources.
  Promise<void> close();

private:
  std::shared_ptr<UvCurlContext_> context_;
};

} // namespace uvco
