
#include "integrations/curl/curl.h"
#include "loop/loop.h"
#include "promise/promise.h"
#include "test_util.h"

#include <curl/curl.h>
#include <fmt/core.h>
#include <gtest/gtest.h>

namespace {

using namespace uvco;

TEST(CurlTest, info) { fmt::print("curl: {}\n", curl_version()); }

TEST(CurlTest, simpleDownload) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    uvco::Curl curl{loop};
    auto gen = curl.download("https://borgac.net");
    while (true) {
      auto maybeValue = co_await gen;
      if (maybeValue.has_value()) {
        fmt::print("Downloaded {} bytes\n", maybeValue->size());
        fmt::print("{}", maybeValue.value());
        continue;
      }
      break;
    }
    co_await curl.close();
  };

  run_loop(setup);
}

} // namespace
