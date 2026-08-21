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

#ifndef VERITAS_FACTS_SOUFFLE_EXPORTER_H_
#define VERITAS_FACTS_SOUFFLE_EXPORTER_H_

#include <filesystem>
#include <span>
#include <vector>

#include "veritas/core/Status.h"
#include "veritas/facts/FactSchema.h"

namespace veritas::facts {

class SouffleExporter {
public:
  static Status WriteBaseRelations(const std::filesystem::path &directory,
                                   std::span<const FactTuple> facts);

  static StatusOr<std::vector<FactTuple>>
  ReadDerivedRelations(const std::filesystem::path &directory,
                       std::span<const FactTuple> base_facts);
};

} // namespace veritas::facts

#endif // VERITAS_FACTS_SOUFFLE_EXPORTER_H_
