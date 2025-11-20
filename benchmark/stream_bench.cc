#include "uvco/pipe.h"
#include <benchmark/benchmark.h>
#include <gtest/gtest.h>

using namespace uvco;

namespace {

void bmPipeReadWrite(benchmark::State &state) {
  runMain<void>([&state](const Loop &loop) -> Promise<void> {
    auto [out, in] = pipe(loop);
    const std::string data(256, 'a');

    for (auto _ : state) {
      co_await in.writeBorrowed(data);
      EXPECT_EQ(data, co_await out.read());
    }
  });
}

BENCHMARK(bmPipeReadWrite);

void bmPipeReadWriteBuffer(benchmark::State &state) {
  runMain<void>([&state](const Loop &loop) -> Promise<void> {
    auto [out, in] = pipe(loop);
    const std::string data(256, 'a');
    std::string buffer(512, 0);

    for (auto _ : state) {
      co_await in.writeBorrowed(data);
      EXPECT_EQ(data.size(), co_await out.read(buffer));
    }
  });
}

BENCHMARK(bmPipeReadWriteBuffer);

} // namespace

BENCHMARK_MAIN();
