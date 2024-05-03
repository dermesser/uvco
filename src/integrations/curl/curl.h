// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include "promise/multipromise.h"
#include "promise/promise.h"

#include <curl/curl.h>
#include <curl/multi.h>

#include <memory>
#include <string>

namespace uvco {

class Loop;

class UvCurlContext_;

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

  /// Download a file from the given URL. The generator will yield the received
  /// chunks.
  MultiPromise<std::string> download(std::string url);

  /// Close the curl handle in order to free all associated resources.
  Promise<void> close();

private:
  std::unique_ptr<UvCurlContext_> context_;
};

} // namespace uvco
