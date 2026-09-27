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

// StoreSummary.h — the store and environment collectors behind a RunReport.
//
// `RunReport.h` is the artifact model and deliberately reaches no further than
// `veritas_core`. The collectors that need a store connection, the M1 manifest,
// or POSIX platform headers live here instead, so a consumer that only fills a
// report still compiles without any of those.
//
// `StoreSummary::cross_checks` is NOT filled here. The store cannot see the
// count the run held in memory, so comparing the two is the caller's job.

#ifndef VERITAS_OBSERVABILITY_STORESUMMARY_H_
#define VERITAS_OBSERVABILITY_STORESUMMARY_H_

#include <filesystem>

#include "veritas/build/AnalysisManifest.h"
#include "veritas/core/Status.h"
#include "veritas/observability/RunReport.h"

namespace veritas::observability {

// CollectStoreSummary reads the published store under `output_root`: one row
// count per counted table, and the on-disk bytes grouped by top-level entry.
//
// Returns NotFound when `output_root` holds no `metadata.db`, and propagates a
// failed count query rather than reporting zero — a table name the schema does
// not define must not read as an empty table.
StatusOr<StoreSummary> CollectStoreSummary(
    const std::filesystem::path& output_root);

// FillEnvironment reads the machine and build identity for the artifact's
// environment block: uname for os/arch, sysctlbyname on Darwin for cores, RAM
// and CPU model, sysconf(_SC_NPROCESSORS_ONLN) and sysinfo on Linux for cores
// and RAM, and this library's own compile definitions for the build identity.
// It deliberately does not reuse the analysis library's build fingerprint,
// which is an identity input.
//
// `cpu_model` is Darwin-only: the Linux branch fills `cores` and `ram_bytes`
// and leaves it empty. An empty string is the honest rendering of "not
// measured here" — do not fill it with a placeholder, and do not let a comment
// claim the Linux branch provides it.
void FillEnvironment(RunEnvironment* environment);

// FillInventoryFromManifest copies the input-scale fields the manifest
// already carries. The manifest types are plain structs in a header, so a
// consumer of this declaration needs no link against veritas_build.
void FillInventoryFromManifest(const build::AnalysisManifest& manifest,
                               RunInputInventory* input);

}  // namespace veritas::observability

#endif  // VERITAS_OBSERVABILITY_STORESUMMARY_H_
