
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

#include "promise/multipromise.h"
#include "promise/promise.h"
#include "run.h"
#include "stream.h"
#include "tcp.h"

struct Options {
  const uvco::Loop *loop;

  bool server = false;
  std::string address = "::1";
  uint16_t port = 9001;
};

Options parseOptions(int argc, const char **argv) {
  namespace po = boost::program_options;
  Options options{};
  bool help = false;
  po::options_description desc;
  desc.add_options()("server", po::bool_switch(&options.server),
                     "act as server")("address",
                                      po::value<std::string>(&options.address),
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

using namespace uvco;

class Hub {
public:
  std::unordered_map<std::string, std::shared_ptr<TcpStream>> clients_;

  Promise<void> broadcast(std::string_view from, std::string_view what) {
    const std::string message = fmt::format("{} says: {}", from, what);
    std::vector<Promise<uv_status>> promises;
    promises.reserve(clients_.size());

    for (const auto &clientPtr : clients_) {
      promises.push_back(clientPtr.second->write(message));
    }
    for (auto &promise : promises) {
      co_await promise;
    }
  }
};

Promise<void> handleConnection(Hub &hub, TcpStream stream) {
  const AddressHandle peer = stream.getPeerName();
  const std::string peerStr{peer.toString()};
  auto streamPtr = std::make_shared<TcpStream>(std::move(stream));

  fmt::print(stderr, "> received connection from {}\n", peerStr);
  hub.clients_[peerStr] = streamPtr;

  while (true) {
    std::optional<std::string> chunk = co_await streamPtr->read();
    if (!chunk) {
      break;
    }
    hub.broadcast(peerStr, std::move(*chunk));
  }
  hub.clients_.erase(peerStr);
  co_await streamPtr->closeReset();
}

Promise<void> server(const Options &opt) {
  AddressHandle bindAddr{opt.address, opt.port};
  TcpServer server{*opt.loop, bindAddr};
  Hub hub;

  MultiPromise<TcpStream> listener = server.listen();

  while (true) {
    std::optional<TcpStream> maybeStream = co_await listener;
    if (!maybeStream) {
      break;
    }
    handleConnection(hub, std::move(*maybeStream));
  }
  co_await server.close();
}

Promise<void> copyIncomingToStdout(const Loop &loop,
                                   std::shared_ptr<TcpStream> conn) {
  TtyStream out = TtyStream::stdout(loop);

  while (true) {
    std::optional<std::string> maybeChunk = co_await conn->read();
    if (!maybeChunk) {
      fmt::print(stderr, "> EOF from connection\n");
      break;
    }
    co_await out.write(std::move(*maybeChunk));
  }

  fmt::print(stderr, "> copier done\n");
  co_await out.close();
  fmt::print(stderr, "> copier really done\n");
}

Promise<void> client(Options opt) {
  TtyStream input = TtyStream::stdin(*opt.loop);
  TcpClient tcpCl{*opt.loop, opt.address, opt.port};
  auto conn = std::make_shared<TcpStream>(co_await tcpCl.connect());

  Promise<void> copier = copyIncomingToStdout(*opt.loop, conn);
  while (true) {
    std::optional<std::string> maybeChunk = co_await input.read();
    if (!maybeChunk) {
      fmt::print(stderr, "> EOF from stdin\n");
      break;
    }
    co_await conn->write(std::move(*maybeChunk));
  }
  fmt::print(stderr, "> loop left\n");
  co_await conn->close();
  fmt::print(stderr, "> conn closed\n");
  co_await copier;
  fmt::print(stderr, "> copier caught\n");
  co_await input.close();
  fmt::print(stderr, "> client done\n");
}

void run(Options opt, const Loop &loop) {
  opt.loop = &loop;

  if (opt.server) {
    server(opt);
  } else {
    client(opt);
  }
}

int main(int argc, const char **argv) {
  Options opt = parseOptions(argc, argv);

  uvco::runMain([&opt](const uvco::Loop &loop) { run(opt, loop); });

  return 0;
}
