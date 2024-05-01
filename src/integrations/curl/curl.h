
#pragma once

#include "promise/multipromise.h"
#include "promise/promise.h"
#include <curl/curl.h>
#include <curl/multi.h>
#include <memory>
#include <string>
#include <uv.h>

namespace uvco {

class Loop;

class UvCurlContext_;

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
