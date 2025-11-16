// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#include "uvco/async_work.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/promise.h"

#include <fmt/core.h>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <utility>

namespace uvco {

/// @addtogroup integrations
/// @{

template <typename R, typename F>
concept WithTxFn = std::is_invocable_r_v<R, F, pqxx::work &>;

/// A thread-pool based implementation of PostgreSQL interactions using the
/// libpqxx library.
class Pqxx {
public:
  explicit Pqxx(const Loop &loop, std::string connectionString)
      : loop_{loop}, connectionString_{std::move(connectionString)} {}

  /// Run a query on the database. The supplied function MUST be thread-safe,
  /// for example, by owning all the data it interacts with.
  ///
  ///
  /// The callable `F` is expected to take a `pqxx::work &` as its only
  /// argument. Its return value is forwarded to the caller.
  template <typename R, typename F>
    requires WithTxFn<R, F>
  Promise<R> withTx(F f);

  /// If you know that you're done with this thread's connection, you can close
  /// it. If needed, a new connection is established again later.
  void close() { conn_.del(); }

private:
  const Loop &loop_;
  ThreadLocalKey<std::optional<pqxx::connection>> conn_;
  std::string connectionString_;
};

template <typename R, typename F>
  requires WithTxFn<R, F>
Promise<R> Pqxx::withTx(F f) {
  // We use one connection per thread-pool thread. That's the best balance
  // between efficiency and safety.
  ThreadLocalKey<std::optional<pqxx::connection>> threadLocalConn{conn_};
  auto connectionString = connectionString_;

  auto work = [threadLocalConn, connectionString = std::move(connectionString),
               f = std::forward<F>(f)]() mutable -> R {
    auto &maybeConnection = threadLocalConn.getOrDefault();
    if (!maybeConnection.has_value()) {
      maybeConnection = std::make_optional(pqxx::connection{connectionString});
    }
    pqxx::work tx{maybeConnection.value()};
    if constexpr (std::is_void_v<R>) {
      f(tx);
      tx.commit();
      return;
    } else {
      R result{f(tx)};
      tx.commit();
      return result;
    }
  };

  return submitWork<R>(loop_, work);
}

/// @}

} // namespace uvco
