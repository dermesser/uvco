#include <gtest/gtest.h>
#include <uv.h>

#include "test_util.h"
#include "uvco/exception.h"

#include <string>

namespace {

using namespace uvco;

// Add tests that don't yet deserve their own file here.

TEST(MiscTest, uvcoException) {
  UvcoException exc("test");
  EXPECT_EQ(exc.what(), std::string("test"));
}

} // namespace
