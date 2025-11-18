/** A toy implementation of the memcached text protocol.
 *
 * Used to demonstrate viability of using uvco for network programming.
 *
 * The parser is trivial and not very robust.
 *
 * The `memcached_client.py` script can be used for issuing simple get/set
 * commands.
 */
#include <boost/log/core.hpp>
#include <boost/log/core/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/trivial.hpp>

#include "uvco/name_resolution.h"
#include "uvco/promise/promise.h"
#include "uvco/run.h"
#include "uvco/tcp.h"
#include "uvco/tcp_stream.h"

#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace uvco;

namespace {

struct Command {
  enum class Type { Get, Set, Delete, Unknown };

  Type type;
  uint32_t flags;
  std::string key;
  std::string value;
  uint64_t exp;

  [[nodiscard]] std::string toString() const {
    return fmt::format(
        "Command{{type: {}, key: '{}', value: '{}', flags: {}, exp: {}}}",
        static_cast<int>(type), key, value, flags, exp);
  }

  std::string process(std::unordered_map<std::string, std::string> &cache) {
    switch (type) {
    case Type::Get: {
      const auto it = cache.find(key);
      if (it == cache.end()) {
        return "NOT_FOUND\r\n";
      }
      return fmt::format("VALUE {} {} {}\r\n{}\r\nEND\r\n", key, flags,
                         it->second.size(), it->second);
    }
    case Type::Set: {
      cache.emplace(std::move(key), std::move(value));
      return "STORED\r\n";
    }
    case Type::Delete: {
      const auto it = cache.find(key);
      if (it == cache.end()) {
        return "NOT_FOUND\r\n";
      }
      cache.erase(it);
      return "DELETED\r\n";
    }
    default:
      return "ERROR\r\n";
    }
  }
};

// Better way to parse memcached commands.
// A command can come in two forms for now:
//
// get <key>\r\n - Get the value for the key.
//
// set <key> <flags> <expiration> <bytes-length>\r\n<value>\r\n - Set the value
std::optional<Command> parseCommand(std::string line) {
  auto commandAndValue = std::views::split(line, std::string_view{"\r\n"}) |
                         std::views::transform([](auto &&range) {
                           return std::string_view{range.begin(), range.end()};
                         });
  auto command = *commandAndValue.begin();
  auto value = *std::next(commandAndValue.begin());
  auto parts = command | std::views::split(' ') |
               std::views::transform([](auto &&range) {
                 return std::string_view{range.begin(), range.end()};
               });

  auto it = parts.begin();
  if (it == parts.end()) {
    return std::nullopt;
  }

  if (*it == "get") {
    ++it;
    if (it == parts.end()) {
      return std::nullopt;
    }
    return Command{.type = Command::Type::Get,
                   .flags = 0,
                   .key = std::string{*it},
                   .value = "",
                   .exp = 0};
  }

  if (*it == "set") {
    ++it;
    if (it == parts.end()) {
      return std::nullopt;
    }
    auto key = *it;
    ++it;
    if (it == parts.end()) {
      return std::nullopt;
    }
    auto flags = std::stoul(std::string{*it});
    ++it;
    if (it == parts.end()) {
      return std::nullopt;
    }
    auto exp = std::stoull(std::string{*it});
    ++it;
    if (it == parts.end()) {
      return std::nullopt;
    }
    return Command{.type = Command::Type::Set,
                   .flags = static_cast<uint32_t>(flags),
                   .key = std::string{key},
                   .value = std::string{value},
                   .exp = exp};
  }

  return std::nullopt;
}

Promise<void> handleClient(TcpStream stream,
                           std::unordered_map<std::string, std::string> &cache,
                           std::string peer) {

  while (true) {
    auto buffer = co_await stream.read();
    if (!buffer) {
      BOOST_LOG_TRIVIAL(info) << "Client disconnected " << peer;
      break;
    }
    BOOST_LOG_TRIVIAL(debug) << "Received " << *buffer << " from " << peer;
    auto cmd = parseCommand(std::move(*buffer));
    if (!cmd) {
      BOOST_LOG_TRIVIAL(debug) << "Client sent invalid command from " << peer;
      continue;
    }
    auto response = cmd->process(cache);
    co_await stream.write(std::move(response));
    BOOST_LOG_TRIVIAL(debug) << "Sent response to " << peer;
  }

  stream.close();
  co_return;
}

Promise<void> mainLoop(const Loop &loop) {
  std::unordered_map<std::string, std::string> cache;
  // Currently: accumulate all clients in a vector. Later: clean up old ones.
  std::vector<Promise<void>> clients;

  const AddressHandle bindAddr{"::1", 9999};
  TcpServer server{loop, bindAddr};
  auto listener = server.listen();

  while (true) {
    auto stream = co_await listener;
    if (!stream) {
      BOOST_LOG_TRIVIAL(info) << "Listener closed";
      break;
    }
    std::string peer = stream->getPeerName().toString();
    BOOST_LOG_TRIVIAL(info) << "Accepted connection from " << peer;
    // Store promise (we won't need it); just calling the function will
    // schedule it for execution.
    clients.push_back(handleClient(std::move(*stream), cache, std::move(peer)));
  }

  co_return;
}

} // namespace

int main() {
  boost::log::core::get()->set_filter(boost::log::trivial::severity >=
                                      boost::log::trivial::info);

  runMain<void>(mainLoop);
  return 0;
}
