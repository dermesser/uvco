
#pragma once

#include <curl/curl.h>
#include <curl/multi.h>
#include <uv.h>

namespace uvco {

struct Loop;

class Curl {
public:
  explicit Curl(const Loop &loop);

  Curl(const Curl &) = default;
  Curl(Curl &&other) noexcept;
  Curl &operator=(const Curl &) = delete;
  Curl &operator=(Curl &&other) noexcept;
  ~Curl() { curl_multi_cleanup(multi_); }

private:
  CURLM *multi_;
  uv_loop_t *loop_;
};

} // namespace uvco
