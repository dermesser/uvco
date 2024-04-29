
#pragma once

#include "promise/multipromise.h"
#include "promise/promise.h"
#include <curl/curl.h>
#include <curl/multi.h>
#include <string>
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
  ~Curl();

  Promise<void> close();
  MultiPromise<std::string> download(std::string url);

  [[nodiscard]] CURLM* multi() const { return multi_; }
  uv_timer_t& timer() { return timer_; }
  uv_loop_t* loop() { return loop_; }

private:
  CURLM *multi_;
  uv_loop_t *loop_;
  uv_timer_t timer_;
};

} // namespace uvco
