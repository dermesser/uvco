
#include "channel.h"

#include <gtest/gtest.h>

namespace {
using namespace uvco;
}

TEST(BQTest, basicPushPop) {
  BoundedQueue<int> bq{4};

  EXPECT_EQ(bq.size(), 0);
  EXPECT_TRUE(bq.empty());

  bq.push(1);
  bq.push(2);

  EXPECT_EQ(bq.size(), 2);
  EXPECT_EQ(bq.pop(), 1);

  bq.push(3);
  bq.push(4);
  
  EXPECT_EQ(bq.size(), 3);
  EXPECT_EQ(bq.pop(), 2);

  bq.push(5);
  bq.push(6);

  EXPECT_EQ(bq.size(), 4);
  EXPECT_EQ(bq.pop(), 3);

  bq.push(7);
  
  EXPECT_EQ(bq.size(), 4);
  EXPECT_EQ(bq.pop(), 4);
}

TEST(BQTest, pushTooMany) {
  BoundedQueue<int> bq(2);
  bq.push(1);
  bq.push(2);
  EXPECT_DEATH({ bq.push(3); }, "size\\(\\) < queue_.capacity");
}

TEST(BQTest, popEmpty) {
  BoundedQueue<int> bq(2);
  bq.push(1);
  bq.push(2);
  EXPECT_EQ(bq.pop(), 1);
  EXPECT_EQ(bq.pop(), 2);
  EXPECT_EQ(bq.size(), 0);
  EXPECT_DEATH({ bq.pop(); }, "size\\(\\) > 0");
}
