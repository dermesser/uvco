
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
    auto gen1 = curl.download("http://borgac.net/");
    auto gen2 = curl.download("https://lewinb.net/");

    try {
      while (true) {
        auto result1 = co_await gen1;
        if (!result1) {
          fmt::print("Downloaded first file\n");
          break;
        }
      }
      while (true) {
        auto result2 = co_await gen2;
        if (!result2) {
          fmt::print("Downloaded second file\n");
          break;
        }
      }

    } catch (const std::exception &e) {
      fmt::print("Caught exception: {}\n", e.what());
    }

    fmt::print("Downloaded both files, closing timer\n");
    co_await curl.close();
    fmt::print("Closed curl handle\n");
  };

  run_loop(setup);
}

Promise<void> provokeError(const Loop& loop, std::string url) {
    uvco::Curl curl{loop};
    auto gen = curl.download(url);

    try {
      while (true) {
        auto result = co_await gen;
        if (!result) {
          fmt::print("Downloaded file\n");
          break;
        }
      }

      EXPECT_FALSE(true) << "Should have thrown an exception";
    } catch (const UvcoException &e) {
      fmt::print("Caught expected exception: {}\n", e.what());
    }

    co_await curl.close();
}

TEST(CurlTest, connectionRefused) {
  run_loop([](const Loop &loop) -> Promise<void> {
    return provokeError(loop, "http://localhost:12345/");
  });
}

TEST(CurlTest, sslVerifyFailed) {
  run_loop([](const Loop &loop) -> Promise<void> {
    return provokeError(loop, "https://abc.borgac.net/");
  });
}

TEST(CurlTest, invalidHost) {
  run_loop([](const Loop &loop) -> Promise<void> {
    return provokeError(loop, "https://borgac-evil-sibling.net/");
  });
}

} // namespace
