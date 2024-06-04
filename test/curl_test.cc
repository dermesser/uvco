#include <curl/curl.h>
#include <fmt/core.h>
#include <gtest/gtest.h>

#include "test_util.h"

#include "uvco/exception.h"
#include "uvco/integrations/curl/curl.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/promise.h"

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

    req2.setTimeoutMs(4000);

    auto gen1 = req1.start();
    auto gen2 = req2.start();

    try {
      while (true) {
        auto result1 = co_await gen1;
        if (!result1) {
          break;
        }
      }
      while (true) {
        auto result2 = co_await gen2;
        if (!result2) {
          break;
        }
      }

    } catch (const CurlException &e) {
      fmt::print("Caught exception: {}\n", e.what());
    }

    co_await curl.close();
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
        break;
      }
    }

    EXPECT_FALSE(true) << "Should have thrown an exception";
  } catch (const CurlException &e) {
    fmt::print("Caught expected exception: {}\n", e.what());
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

    while (true) {
      auto result = co_await gen;
      if (!result) {
        break;
      }
    }

    EXPECT_EQ(405, req.statusCode().value());

    co_await curl.close();
  });
}

} // namespace
