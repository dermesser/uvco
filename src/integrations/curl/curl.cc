
#include "integrations/curl/curl.h"
#include "loop/loop.h"
#include <curl/multi.h>

namespace uvco {

Curl &Curl::operator=(Curl &&other) noexcept {
  if (this != &other) {
    multi_ = other.multi_;
    loop_ = other.loop_;
    other.multi_ = nullptr;
    other.loop_ = nullptr;
  }
  return *this;
}

Curl::Curl(Curl &&other) noexcept {
  if (this != &other) {
    multi_ = other.multi_;
    loop_ = other.loop_;
    other.multi_ = nullptr;
    other.loop_ = nullptr;
  }
}

Curl::Curl(const Loop &loop)
    : multi_{curl_multi_init()}, loop_{loop.uvloop()} {}

} // namespace uvco
