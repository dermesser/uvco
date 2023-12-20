
#include "timer.h"
#include <boost/program_options.hpp>
#include <boost/program_options/variables_map.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include <scheduler.h>
#include <udp.h>

using namespace uvco;

struct Options {
  uv_loop_t *loop;

  std::string address = "239.253.1.1";
  uint16_t port = 8012;
};

Options parseOptions(int argc, const char **argv) {
  namespace po = boost::program_options;
  Options options{};
  bool help = false;
  po::options_description desc;
  desc.add_options()("address", po::value<std::string>(&options.address),
                     "Listen/connect address")(
      "port", po::value<uint16_t>(&options.port),
      "Listen/connect port")("help,h", po::bool_switch(&help), "Display help");
  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);
  if (help) {
    std::cerr << desc << std::endl;
    std::exit(0);
  }
  return options;
}

Promise<void> sendSome(const Options &opt, AddressHandle to, size_t n = 5,
                       int interval = 1) {
  Udp udp{opt.loop};

  for (size_t i = 0; i < n; i++) {
    co_await udp.send(std::string_view{"hello back"}, to);
    //co_await wait(opt.loop, 100 * interval);
  }
  co_await udp.close();
}

Promise<void> printPackets(const Options &opt) {
  Udp udp{opt.loop};
  std::vector<Promise<void>> active;

  try {
    co_await udp.bind(opt.address, 8012);

    udp.joinMulticast(opt.address, "192.168.1.66");
    udp.setMulticastLoop(false);
    AddressHandle dst{opt.address, opt.port};

    constexpr static std::string_view hello = "hello first";
    udp.send(hello, dst);

      fmt::print(stderr, "waiting for packets\n");
    while (true) {
      const auto [packet, from] = co_await udp.receiveOneFrom();
      fmt::print("Received packet: {} from {}\n", packet, from.toString());
      sendSome(opt, from);
    }
  } catch (const UvcoException &e) {
    fmt::print(stderr, "exception: {}\n", e.what());
  }
  co_await udp.close();
}

void run(Options opt) {
  LoopData data;

  uv_loop_t loop;
  uv_loop_init(&loop);
  uv_loop_set_data(&loop, &data);
  data.setUpLoop(&loop);

  opt.loop = &loop;

  Promise<void> _ = printPackets(opt);

  uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);
}

int main(int argc, const char **argv) {
  Options opt = parseOptions(argc, argv);

  run(opt);
}
