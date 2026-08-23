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

#include <gtest/gtest.h>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <memory>
#include <span>
#include <string>

#include "analysis/llvm/AbstractMemoryBuilder.h"
#include "analysis/llvm/OriginMap.h"
#include "analysis/llvm/StableValueMapper.h"
#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/core/Ids.h"

namespace veritas::analysis::llvm {
namespace {

using namespace veritas::analysis::semantic;

constexpr const char* kDataLayout =
    "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64"
    "-f32:32:32-f64:64:64";

OriginMap OriginMapFor(const ::llvm::Module& module) {
  OriginMap origin_map;
  for (const auto& function : module) {
    if (function.isDeclaration())
      continue;
    const std::string name = function.getName().str();
    const auto id = core::MakeStableId(
        core::IdKind::kFunctionVariant,
        std::as_bytes(std::span(name.data(), name.size())));
    origin_map.RecordOrigin(const_cast<::llvm::Function*>(&function),
                            core::ToString(id));
  }
  return origin_map;
}

const ::llvm::Value* FindFirstGep(const ::llvm::Module& module,
                                const char* function_name) {
  const ::llvm::Function* function = module.getFunction(function_name);
  if (!function)
    return nullptr;
  for (const auto& block : *function) {
    for (const auto& inst : block) {
      if (const auto* gep = ::llvm::dyn_cast<::llvm::GetElementPtrInst>(&inst)) {
        return gep;
      }
    }
  }
  return nullptr;
}

// Owns everything the mapper/builder reference. Heap-owning the module, data
// layout, origin map, and mapper keeps their addresses stable when the fixture
// is moved, so the builder's internal references stay valid.
struct MemoryFixture {
  std::unique_ptr<::llvm::LLVMContext> context;
  std::unique_ptr<::llvm::Module> module;
  std::unique_ptr<::llvm::DataLayout> layout;
  std::unique_ptr<OriginMap> origin_map;
  std::unique_ptr<StableValueMapper> mapper;
  AbstractMemoryBuilder builder;
  const ::llvm::Value* pointer;

  MemoryFixture(std::unique_ptr<::llvm::LLVMContext> ctx,
                std::unique_ptr<::llvm::Module> mod,
                std::unique_ptr<::llvm::DataLayout> dl,
                std::unique_ptr<OriginMap> origins, const ::llvm::Value* ptr)
      : context(std::move(ctx)),
        module(std::move(mod)),
        layout(std::move(dl)),
        origin_map(std::move(origins)),
        mapper(std::make_unique<StableValueMapper>(*module, *origin_map)),
        builder(*layout, *mapper, *origin_map),
        pointer(ptr) {}
};

MemoryFixture BuildFixture(const char* ir, const char* function_name) {
  auto context = std::make_unique<::llvm::LLVMContext>();
  ::llvm::SMDiagnostic error;
  auto module = ::llvm::parseAssemblyString(ir, error, *context);
  if (!module) {
    error.print("AbstractMemoryBuilderTest", ::llvm::errs());
    std::abort();
  }
  module->setDataLayout(kDataLayout);
  const ::llvm::Value* gep = FindFirstGep(*module, function_name);
  auto layout = std::make_unique<::llvm::DataLayout>(module->getDataLayout());
  auto origins = std::make_unique<OriginMap>(OriginMapFor(*module));
  return MemoryFixture(std::move(context), std::move(module),
                       std::move(layout), std::move(origins), gep);
}

MemoryFixture FieldStoreFixture() {
  // A non-homogeneous struct: field 0 is i8, so field 1 (i32) sits at a
  // non-zero byte offset (4), not `field_index * 4`.
  return BuildFixture(R"(
    %Record = type { i8, i32 }
    define void @store_field(ptr %rec) {
      %gep = getelementptr inbounds %Record, ptr %rec, i32 0, i32 1
      store i32 0, ptr %gep
      ret void
    }
  )", "store_field");
}

MemoryFixture NonHomogeneousFixture() {
  // Another non-homogeneous struct: field 1 (i64) sits at byte offset 8.
  return BuildFixture(R"(
    %Wide = type { i16, i64 }
    define void @store_wide(ptr %rec) {
      %gep = getelementptr inbounds %Wide, ptr %rec, i32 0, i32 1
      store i64 0, ptr %gep
      ret void
    }
  )", "store_wide");
}

MemoryFixture VariableIndexFixture() {
  return BuildFixture(R"(
    define void @indexed(ptr %arr, i64 %i) {
      %gep = getelementptr inbounds i32, ptr %arr, i64 %i
      store i32 0, ptr %gep
      ret void
    }
  )", "indexed");
}

TEST(AbstractMemoryBuilderTest, PreservesFieldAndByteRange) {
  auto access = FieldStoreFixture();
  auto location = access.builder.LocationFor(*access.pointer, 4);
  ASSERT_TRUE(location.ok());
  // The leading `i32 0` is the outer sequential index (kArrayIndex), and the
  // real struct field is the second index. For `{ i8, i32 }` that field is at
  // byte offset 4, not 1 — this fails if the leading index is misread as a
  // struct field.
  ASSERT_EQ(location->access_path.size(), 2u);
  EXPECT_EQ(location->access_path[0].kind,
            AccessPathSegment::Kind::kArrayIndex);
  EXPECT_EQ(location->access_path[1].kind,
            AccessPathSegment::Kind::kField);
  EXPECT_EQ(location->access_path[1].first, 1);
  EXPECT_EQ(location->byte_range.offset, 4);
  EXPECT_EQ(location->byte_range.size, 4u);
}

TEST(AbstractMemoryBuilderTest, NonHomogeneousStructFieldOffset) {
  auto access = NonHomogeneousFixture();
  auto location = access.builder.LocationFor(*access.pointer, 8);
  ASSERT_TRUE(location.ok());
  ASSERT_EQ(location->access_path.size(), 2u);
  EXPECT_EQ(location->access_path[1].kind,
            AccessPathSegment::Kind::kField);
  EXPECT_EQ(location->access_path[1].first, 1);
  EXPECT_EQ(location->byte_range.offset, 8);
  EXPECT_EQ(location->byte_range.size, 8u);
}

TEST(AbstractMemoryBuilderTest, RepresentsUnknownSuffixExplicitly) {
  auto access = VariableIndexFixture();
  auto location = access.builder.LocationFor(*access.pointer, 4);
  ASSERT_TRUE(location.ok());
  EXPECT_EQ(location->access_path.back().kind,
            AccessPathSegment::Kind::kUnknown);
}

}  // namespace
}  // namespace veritas::analysis::llvm
