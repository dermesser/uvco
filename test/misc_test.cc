#include <gtest/gtest.h>
#include <string>
#include <uv.h>

#include "exception.h"
#include "test_util.h"

namespace {

using namespace uvco;

// Add tests that don't yet deserve their own file here.

TEST(MiscTest, uvcoException) {
  UvcoException exc("test");
  EXPECT_EQ(exc.what(), std::string("test"));
}

} // namespace
