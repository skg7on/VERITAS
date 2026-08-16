// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The VERITAS Authors.
#include "veritas/core/Status.h"

#include <string>
#include <utility>

#include <gtest/gtest.h>

using ::veritas::Status;
using ::veritas::StatusCode;
using ::veritas::StatusOr;

TEST(StatusTest, DefaultConstructedStatusIsOk) {
  Status status;
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kOk);
  EXPECT_EQ(status.message(), "");
}

TEST(StatusTest, OkFactoryIsOk) {
  Status status = Status::Ok();
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kOk);
}

TEST(StatusTest, InvalidArgumentCarriesMessage) {
  Status status = Status::InvalidArgument("bad input");
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(status.message(), "bad input");
}

TEST(StatusTest, NotFoundIsDistinctFromInternal) {
  EXPECT_EQ(Status::NotFound("").code(), StatusCode::kNotFound);
  EXPECT_EQ(Status::Internal("").code(), StatusCode::kInternal);
  EXPECT_EQ(Status::FailedPrecondition("").code(),
            StatusCode::kFailedPrecondition);
}

TEST(StatusOrTest, HoldsValue) {
  StatusOr<int> result(42);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), 42);
  EXPECT_EQ(*result, 42);
  EXPECT_TRUE(result.status().ok());
}

TEST(StatusOrTest, HoldsError) {
  StatusOr<int> result(Status::NotFound("missing"));
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kNotFound);
  EXPECT_EQ(result.status().message(), "missing");
}

TEST(StatusOrTest, MovesValueOut) {
  StatusOr<std::string> result(std::string("payload"));
  ASSERT_TRUE(result.ok());
  std::string moved = std::move(result).value();
  EXPECT_EQ(moved, "payload");
}
