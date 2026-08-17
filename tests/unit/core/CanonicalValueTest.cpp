// Copyright 2026 VERITAS Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "veritas/core/CanonicalValue.h"

#include <gtest/gtest.h>

using namespace veritas::core;

TEST(CanonicalValueTest, SortsMapKeys) {
  auto left = Object({{"b", String("2")}, {"a", String("1")}});
  auto right = Object({{"a", String("1")}, {"b", String("2")}});
  EXPECT_EQ(CanonicalEncode(left).value(), CanonicalEncode(right).value());
}

TEST(CanonicalValueTest, PreservesArrayOrder) {
  auto left = Array({String("first"), String("second")});
  auto right = Array({String("second"), String("first")});
  EXPECT_NE(CanonicalEncode(left).value(), CanonicalEncode(right).value());
}

TEST(CanonicalValueTest, EncodesNull) {
  auto value = Null();
  auto result = CanonicalEncode(value);
  EXPECT_TRUE(result.ok());
  EXPECT_FALSE(result.value().empty());
}

TEST(CanonicalValueTest, EncodesBoolean) {
  auto true_value = Bool(true);
  auto false_value = Bool(false);
  EXPECT_NE(CanonicalEncode(true_value).value(),
            CanonicalEncode(false_value).value());
}

TEST(CanonicalValueTest, EncodesInteger) {
  auto value = Int(42);
  auto result = CanonicalEncode(value);
  EXPECT_TRUE(result.ok());
  EXPECT_FALSE(result.value().empty());
}

TEST(CanonicalValueTest, EncodesString) {
  auto value = String("hello");
  auto result = CanonicalEncode(value);
  EXPECT_TRUE(result.ok());
  EXPECT_FALSE(result.value().empty());
}

TEST(CanonicalValueTest, EncodesNestedObject) {
  auto inner = Object({{"nested", String("value")}});
  auto outer = Object({{"outer", inner}});
  auto result = CanonicalEncode(outer);
  EXPECT_TRUE(result.ok());
}

TEST(CanonicalValueTest, EncodesTaggedPath) {
  TaggedPath path{PathRootKind::kRepository, "root-id", "src/main.cpp"};
  auto value = Path(path);
  auto result = CanonicalEncode(value);
  EXPECT_TRUE(result.ok());
  EXPECT_FALSE(result.value().empty());
}

TEST(CanonicalValueTest, DifferentPathRootKindsProduceDifferentBytes) {
  TaggedPath repo_path{PathRootKind::kRepository, "root", "file.cpp"};
  TaggedPath external_path{PathRootKind::kExternal, "root", "file.cpp"};
  auto repo_encoded = CanonicalEncode(Path(repo_path)).value();
  auto external_encoded = CanonicalEncode(Path(external_path)).value();
  EXPECT_NE(repo_encoded, external_encoded);
}
