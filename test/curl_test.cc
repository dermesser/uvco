
#include "exception.h"
#include "integrations/curl/curl.h"
#include "loop/loop.h"
#include "promise/promise.h"
#include "test_util.h"

#include <curl/curl.h>
#include <fmt/core.h>
#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace uvco;

TEST(CurlTest, info) { fmt::print("curl: {}\n", curl_version()); }

TEST(CurlTest, simpleDownload) {
  auto setup = [](const Loop &loop) -> Promise<void> {
    Curl curl{loop};
    auto req1 = curl.get("https://borgac.net/");
    auto req2 = curl.get("http://lewinb.net/");
    auto gen1 = req1.start();
    auto gen2 = req2.start();

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

    } catch (const UvcoException &e) {
      fmt::print("Caught exception: {}\n", e.what());
    }

    fmt::print("Downloaded both files, closing timer\n");
    co_await curl.close();
    fmt::print("Closed curl handle\n");
  };

  run_loop(setup);
}

Promise<void> provokeError(const Loop &loop, std::string url) {
  Curl curl{loop};
  auto req = curl.get(url);
  auto gen = req.start();

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

TEST(CurlTest, invalidPost) {
  run_loop([](const Loop &loop) -> Promise<void> {
    Curl curl{loop};
    const std::vector<std::pair<std::string, std::string>> fields = {
        {"key1", "value1"}, {"key2", "value2"}};
    auto req = curl.post("https://borgac.net/", fields);
    auto gen = req.start();

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
      EXPECT_TRUE(std::string(e.what()).contains("HTTP Error 405"));
    }

    EXPECT_EQ(req.statusCode(), 405);

    co_await curl.close();
  });
}

} // namespace
