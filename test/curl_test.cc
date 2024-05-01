
#include "exception.h"
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
    auto gen1 = curl.download("https://borgac.net/~lbo/");
    auto gen2 = curl.download("http://ip4.me/");

    while (true) {
      auto result2 = co_await gen2;
      if (!result2) {
        fmt::print("Downloaded second file\n");
        break;
      }
    }

    while (true) {
      auto result1 = co_await gen1;
      if (!result1) {
        fmt::print("Downloaded first file\n");
        break;
      }
    }

    fmt::print("Downloaded both files, closing timer\n");
    co_await curl.close();
    fmt::print("Closed curl handle\n");
  };

  run_loop(setup);
}

} // namespace
