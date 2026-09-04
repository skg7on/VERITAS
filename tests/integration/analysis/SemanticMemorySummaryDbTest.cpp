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

#include "WpaFixtureHarness.h"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/analysis/ProjectAnalyzer.h"
#include "veritas/summary/v2/summary.pb.h"

namespace veritas::testing {
namespace {

namespace v2 = summary::v2;
namespace v1 = summary::v1;

const v2::FunctionSummary* SummaryForSymbol(
    const AnalyzedFixtureSnapshot& snapshot, std::string_view symbol) {
  const auto id = snapshot.function_variant_ids_by_symbol.find(
      std::string(symbol));
  if (id == snapshot.function_variant_ids_by_symbol.end()) {
    return nullptr;
  }
  for (const auto& artifact : snapshot.summaries) {
    const auto* summary = std::get_if<v2::FunctionSummary>(&artifact);
    if (summary != nullptr &&
        summary->identity().function_variant_id() == id->second) {
      return summary;
    }
  }
  return nullptr;
}

bool HasEffect(const v2::FunctionSummary& summary, v1::EffectKind kind,
               v2::AbstractObjectKind object_kind, std::int64_t offset,
               std::uint64_t size) {
  for (const auto& effect : summary.memory_effects()) {
    const auto& location = effect.location();
    const auto& range = location.byte_range();
    if (effect.kind() == kind && location.object().kind() == object_kind &&
        range.offset_known() && range.size_known() &&
        range.offset() == offset && range.size() == size) {
      return true;
    }
  }
  return false;
}

std::vector<const v2::MemoryLocation*> MemoryLocations(
    const std::vector<summary::SummaryArtifact>& summaries) {
  std::vector<const v2::MemoryLocation*> locations;
  for (const auto& artifact : summaries) {
    const auto* function = std::get_if<v2::FunctionSummary>(&artifact);
    if (function == nullptr) {
      continue;
    }
    for (const auto& effect : function->memory_effects()) {
      locations.push_back(&effect.location());
    }
  }
  return locations;
}

bool HasObjectKinds(const std::vector<summary::SummaryArtifact>& summaries,
                    std::initializer_list<v2::AbstractObjectKind> expected) {
  std::set<v2::AbstractObjectKind> found;
  for (const auto* location : MemoryLocations(summaries)) {
    found.insert(location->object().kind());
  }
  for (const auto kind : expected) {
    if (!found.contains(kind)) {
      return false;
    }
  }
  return true;
}

bool HasFieldAndArrayAccessPaths(
    const std::vector<summary::SummaryArtifact>& summaries) {
  bool field = false;
  bool array = false;
  for (const auto* location : MemoryLocations(summaries)) {
    for (const auto& segment : location->access_path()) {
      field = field || segment.kind() == v2::AccessPathSegment::KIND_FIELD;
      array = array || segment.kind() == v2::AccessPathSegment::KIND_ARRAY_INDEX;
    }
  }
  return field && array;
}

bool HasKnownZeroOffsetRange(
    const std::vector<summary::SummaryArtifact>& summaries) {
  for (const auto* location : MemoryLocations(summaries)) {
    const auto& range = location->byte_range();
    if (range.offset_known() && range.size_known() && range.offset() == 0) {
      return true;
    }
  }
  return false;
}

bool HasCanonicalUnknownRange(
    const std::vector<summary::SummaryArtifact>& summaries) {
  for (const auto* location : MemoryLocations(summaries)) {
    const auto& range = location->byte_range();
    if (!range.offset_known() && !range.size_known() && range.offset() == 0 &&
        range.size() == 0) {
      return true;
    }
  }
  return false;
}

std::string MemoryObservations(
    const std::vector<summary::SummaryArtifact>& summaries) {
  std::string text;
  for (const auto* location : MemoryLocations(summaries)) {
    if (!text.empty()) {
      text += ", ";
    }
    text += std::to_string(static_cast<int>(location->object().kind()));
    text += "/";
    text += std::to_string(location->byte_range().offset_known());
    text += "/";
    text += std::to_string(location->byte_range().offset());
    text += "/";
    text += std::to_string(location->byte_range().size_known());
    text += "/";
    text += std::to_string(location->byte_range().size());
  }
  return text;
}

TEST(SemanticMemorySummaryDbTest, StructuredMemorySurvivesV2Persistence) {
  auto config = analysis::AnalysisConfig::Default();
  config.wpa_engine = analysis::WpaEngineMode::kCppEmergency;
  auto snapshot = AnalyzeAndLoadFixture("abstract_memory_semantic", config);
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
  EXPECT_TRUE(AllArtifactsAreV2(snapshot->summaries));
  EXPECT_TRUE(HasObjectKinds(snapshot->summaries,
      {v2::ABSTRACT_OBJECT_KIND_GLOBAL, v2::ABSTRACT_OBJECT_KIND_STACK,
       v2::ABSTRACT_OBJECT_KIND_ARGUMENT}))
      << MemoryObservations(snapshot->summaries);
  EXPECT_TRUE(HasFieldAndArrayAccessPaths(snapshot->summaries));
  EXPECT_TRUE(HasKnownZeroOffsetRange(snapshot->summaries))
      << MemoryObservations(snapshot->summaries);
  EXPECT_TRUE(HasCanonicalUnknownRange(snapshot->summaries));
}

TEST(SemanticMemorySummaryDbTest, KnownZeroRangeStaysDistinctFromUnknown) {
  const auto known_zero = analysis::semantic::ByteRange::Known(0, 0);
  const auto unknown = analysis::semantic::ByteRange::Unknown();
  EXPECT_NE(known_zero, unknown);
  EXPECT_TRUE(known_zero.offset.has_value());
  EXPECT_TRUE(known_zero.size.has_value());
  EXPECT_FALSE(unknown.offset.has_value());
  EXPECT_FALSE(unknown.size.has_value());
}

TEST(SemanticMemorySummaryDbTest, FixtureOperationsKeepScopedMemoryMeaning) {
  auto config = analysis::AnalysisConfig::Default();
  config.wpa_engine = analysis::WpaEngineMode::kCppEmergency;
  auto snapshot = AnalyzeAndLoadFixture("abstract_memory_semantic", config);
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();

  const auto* zero = SummaryForSymbol(*snapshot, "memory_zero_range");
  ASSERT_NE(zero, nullptr);
  EXPECT_TRUE(HasEffect(*zero, v1::EFFECT_KIND_READ,
                        v2::ABSTRACT_OBJECT_KIND_ARGUMENT, 0, 4));

  const auto* nested = SummaryForSymbol(*snapshot, "memory_nested");
  ASSERT_NE(nested, nullptr);
  bool nested_field = false;
  bool nested_array = false;
  bool nested_stack = false;
  bool nested_argument = false;
  for (const auto& effect : nested->memory_effects()) {
    nested_stack = nested_stack ||
        effect.location().object().kind() == v2::ABSTRACT_OBJECT_KIND_STACK;
    nested_argument = nested_argument ||
        effect.location().object().kind() == v2::ABSTRACT_OBJECT_KIND_ARGUMENT;
    for (const auto& segment : effect.location().access_path()) {
      nested_field = nested_field ||
          (segment.kind() == v2::AccessPathSegment::KIND_FIELD &&
           segment.first() == 1);
      nested_array = nested_array ||
          (segment.kind() == v2::AccessPathSegment::KIND_ARRAY_INDEX &&
           segment.first() == 2);
    }
  }
  EXPECT_TRUE(nested_field);
  EXPECT_TRUE(nested_array);
  EXPECT_TRUE(nested_stack);
  EXPECT_TRUE(nested_argument);

  const auto* constant = SummaryForSymbol(*snapshot, "memory_constant_index");
  ASSERT_NE(constant, nullptr);
  EXPECT_TRUE(HasEffect(*constant, v1::EFFECT_KIND_READ,
                        v2::ABSTRACT_OBJECT_KIND_ARGUMENT, 12, 4));
  bool constant_index = false;
  for (const auto& effect : constant->memory_effects()) {
    for (const auto& segment : effect.location().access_path()) {
      if (segment.kind() == v2::AccessPathSegment::KIND_ARRAY_INDEX &&
          segment.first() == 3 && segment.last() == 3) {
        constant_index = true;
      }
    }
  }
  EXPECT_TRUE(constant_index);

  const auto* variable = SummaryForSymbol(*snapshot, "memory_variable_index");
  ASSERT_NE(variable, nullptr);
  bool variable_unknown = false;
  for (const auto& effect : variable->memory_effects()) {
    const auto& location = effect.location();
    const auto& range = location.byte_range();
    if (location.object().kind() == v2::ABSTRACT_OBJECT_KIND_ARGUMENT &&
        !range.offset_known() && !range.size_known() && range.offset() == 0 &&
        range.size() == 0 && !location.access_path().empty() &&
        location.access_path(location.access_path_size() - 1).kind() ==
            v2::AccessPathSegment::KIND_UNKNOWN) {
      variable_unknown = true;
    }
  }
  EXPECT_TRUE(variable_unknown);

  const auto* overlap = SummaryForSymbol(*snapshot, "memory_overlap");
  ASSERT_NE(overlap, nullptr);
  const v2::MemoryLocation* whole = nullptr;
  const v2::MemoryLocation* byte = nullptr;
  for (const auto& effect : overlap->memory_effects()) {
    const auto& location = effect.location();
    if (effect.kind() == v1::EFFECT_KIND_WRITE &&
        location.object().kind() == v2::ABSTRACT_OBJECT_KIND_ARGUMENT &&
        location.byte_range().offset_known() &&
        location.byte_range().size_known() &&
        location.byte_range().offset() == 0 && location.byte_range().size() == 4) {
      whole = &location;
    }
    if (effect.kind() == v1::EFFECT_KIND_READ &&
        location.object().kind() == v2::ABSTRACT_OBJECT_KIND_ARGUMENT &&
        location.byte_range().offset_known() &&
        location.byte_range().size_known() &&
        location.byte_range().offset() == 0 && location.byte_range().size() == 1) {
      byte = &location;
    }
  }
  ASSERT_NE(whole, nullptr);
  ASSERT_NE(byte, nullptr);
  EXPECT_EQ(whole->object().abstract_object_id(),
            byte->object().abstract_object_id());
  EXPECT_NE(whole->memory_location_id(), byte->memory_location_id());

  const auto* global = SummaryForSymbol(*snapshot, "memory_global");
  ASSERT_NE(global, nullptr);
  std::set<std::string> global_objects;
  std::set<std::string> global_names;
  for (const auto& effect : global->memory_effects()) {
    if (effect.location().object().kind() == v2::ABSTRACT_OBJECT_KIND_GLOBAL) {
      global_objects.insert(effect.location().object().abstract_object_id());
      global_names.insert(effect.location().object().diagnostic_name());
    }
  }
  EXPECT_EQ(global_objects.size(), 2u);
  EXPECT_EQ(global_names,
            (std::set<std::string>{"memory_global_value",
                                   "memory_static_value"}));
}

TEST(SemanticMemorySummaryDbTest, MemoryAndObjectIdentitiesStayDistinct) {
  auto config = analysis::AnalysisConfig::Default();
  config.wpa_engine = analysis::WpaEngineMode::kCppEmergency;
  auto snapshot = AnalyzeAndLoadFixture("abstract_memory_semantic", config);
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();

  std::set<std::string> locations;
  std::set<std::string> objects;
  for (const auto* location : MemoryLocations(snapshot->summaries)) {
    locations.insert(location->memory_location_id());
    objects.insert(location->object().abstract_object_id());
  }
  EXPECT_GT(locations.size(), 1u);
  EXPECT_GT(objects.size(), 1u);
}

}  // namespace
}  // namespace veritas::testing
