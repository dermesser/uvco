#include "test_util.h"
#include "uvco/integrations/pqxx/pqxx.h"
#include "uvco/loop/loop.h"
#include "uvco/promise/promise.h"

#include <fmt/core.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace uvco;

namespace {

constexpr std::string_view schemaSetup = R"(
CREATE TABLE test (id SERIAL PRIMARY KEY, value TEXT);
INSERT INTO test (value) VALUES ('hello'), ('world');
)";

constexpr std::string_view schemaTearDown = R"(
DROP TABLE test;
)";

constexpr std::string_view connectionString = "dbname=lbo user=lbo";

// This test obviously only runs if you have a PostgreSQL server running. Change
// the connection string to match your setup.
TEST(PqxxTest, DISABLED_basic) {
  auto doSetup = [](pqxx::work &tx) { tx.exec(schemaSetup); };
  auto doTeardown = [](pqxx::work &tx) { tx.exec(schemaTearDown); };

  auto setup = [&doTeardown, &doSetup](const Loop &loop) -> Promise<void> {
    Pqxx pqxx{loop, std::string{connectionString}};

    co_await pqxx.withTx<void>(doSetup);

    auto txn = [](pqxx::work &tx) -> unsigned {
      return tx.query_value<unsigned>("SELECT COUNT(*) FROM test;");
    };
    auto promise = pqxx.withTx<unsigned>(txn);
    EXPECT_EQ(2, co_await promise);

    co_await pqxx.withTx<void>(doTeardown);
    pqxx.close();
    co_return;
  };

  run_loop(setup);
}

} // namespace
