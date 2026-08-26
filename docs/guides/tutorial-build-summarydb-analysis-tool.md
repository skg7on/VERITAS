# Tutorial: Build a SummaryDB Analysis Tool

This tutorial builds a small in-tree C++ tool over APIs available today. The
tool first lists current summaries without assuming v1 or v2, then opens the
current native CPG and queries callees. The final section shows how the existing
diff/dependency APIs support incremental analysis tools.

The tutorial is intentionally API-first. SQL examples in the generation manual
are useful for diagnostics, but product tooling should depend on semantic APIs
so backend layout can change.

## 1. Generate input data

Build VERITAS and analyze a C/C++ project as described in the
[generation manual](summarydb-generation-manual.md):

```bash
build/bin/veritas-build analyze \
  --project /absolute/path/to/project \
  --output /absolute/path/to/summarydb
```

Record the printed revision and build-variant IDs. This tutorial refers to
them as `<revision-id>` and `<build-variant-id>`.

## 2. Write a version-neutral summary lister

Add a temporary in-tree source file such as
`src/tools/veritas-summary-list.cpp`. Repository source files require the full
Apache-2.0 header; it is omitted below only to keep the example focused.

```cpp
#include <iostream>
#include <string>

#include "veritas/core/Hash.h"
#include "veritas/core/Ids.h"
#include "veritas/summary/SummaryArtifact.h"
#include "veritas/summarydb/SummaryRepository.h"

int main(int argc, char* argv[]) {
  if (argc != 4) {
    std::cerr << "usage: veritas-summary-list <db> <revision-id> "
                 "<build-variant-id>\n";
    return 1;
  }

  auto repository =
      veritas::summarydb::SummaryRepository::Open(argv[1]);
  if (!repository.ok()) {
    std::cerr << repository.status().message() << '\n';
    return 1;
  }

  auto artifacts = (*repository)->ListCurrentSummaryArtifacts(argv[2], argv[3]);
  if (!artifacts.ok()) {
    std::cerr << artifacts.status().message() << '\n';
    return 1;
  }

  for (const auto& artifact : *artifacts) {
    auto summary_id =
        veritas::summary::ComputeFunctionSummaryId(artifact);
    if (!summary_id.ok()) {
      std::cerr << summary_id.status().message() << '\n';
      return 1;
    }

    const auto& identity = veritas::summary::Identity(artifact);
    std::cout << veritas::core::ToString(*summary_id) << '\t'
              << veritas::summary::SchemaVersion(artifact) << '\t'
              << identity.function_variant_id() << '\n';
  }

  return 0;
}
```

Why `SummaryArtifact` matters:

- the current analyzer publishes `summary.v2`;
- a persistent repository can also retain v1 history;
- `ListCurrentSummaries` is a v1-only compatibility API and rejects a current
  v2 binding; and
- the artifact helpers dispatch validation, serialization, identity, and
  component hashing through the correct concrete schema.

## 3. Add the in-tree target

Add the target next to the existing CLI definitions in
`src/tools/CMakeLists.txt`:

```cmake
add_executable(veritas-summary-list veritas-summary-list.cpp)
target_link_libraries(
  veritas-summary-list
  PRIVATE
    veritas_core
    veritas_cpg
    veritas_summary
    veritas_summarydb
)
veritas_add_warnings(veritas-summary-list)
```

Build and run:

```bash
cmake --build --preset default --target veritas-summary-list
build/bin/veritas-summary-list \
  /absolute/path/to/summarydb \
  <revision-id> \
  <build-variant-id>
```

Expected output has one stable summary ID, schema version, and function-variant
ID per current binding:

```text
summary:sha256:<digest>  summary.v2  funcvar:sha256:<digest>
```

Do not parse the text prefix to dispatch schema behavior. The typed ID parser
and `SummaryArtifact` variant are the dispatch boundaries.

## 4. Inspect component digests

Extend the loop to show the independent semantic/evidence digests:

```cpp
for (const auto& digest :
     veritas::summary::ComputeComponentDigests(artifact)) {
  std::cout << "  component="
            << veritas::summary::v1::ComponentKind_Name(digest.kind)
            << " items=" << digest.item_count
            << " semantic="
            << veritas::core::DigestToHex(digest.semantic_hash)
            << " evidence="
            << veritas::core::DigestToHex(digest.evidence_hash) << '\n';
}
```

Component digests are the correct basis for scheduling downstream work:

```text
semantic hash changed -> analysis consumer may need recomputation
evidence hash changed -> Evidence/explanation may need refresh
both unchanged        -> stop propagation for this component
```

Do not compare serialized summary blobs and invalidate every caller. That
would discard the central benefit of SummaryDB.

## 5. Add a native CPG callee query

The CPG repository shares the SQLite metadata store with the summary
repository. Open it on one immutable current projection:

```cpp
#include <iostream>
#include <string>

#include "veritas/core/Ids.h"
#include "veritas/cpg/CpgQuery.h"
#include "veritas/cpg/CpgRepository.h"
#include "veritas/summarydb/MetadataStore.h"

int PrintCallees(const std::string& db_root,
                 const std::string& revision_text,
                 const std::string& build_text,
                 const std::string& function_text) {
  auto revision = veritas::core::ParseStableId(revision_text);
  auto build = veritas::core::ParseStableId(build_text);
  auto function = veritas::core::ParseStableId(function_text);
  if (!revision.ok() || !build.ok() || !function.ok()) {
    std::cerr << "invalid stable ID\n";
    return 1;
  }

  auto metadata = veritas::summarydb::MetadataStore::Open(
      db_root + "/metadata.db");
  if (!metadata.ok()) {
    std::cerr << metadata.status().message() << '\n';
    return 1;
  }
  if (auto status = metadata->ApplySchema(); !status.ok()) {
    std::cerr << status.message() << '\n';
    return 1;
  }

  veritas::cpg::CpgRepository graphs(*metadata);
  auto query = veritas::cpg::CpgQuery::OpenCurrent(
      graphs, *revision, *build);
  if (!query.ok()) {
    std::cerr << query.status().message() << '\n';
    return 1;
  }

  auto callees = query->GetCallees(*function);
  if (!callees.ok()) {
    std::cerr << callees.status().message() << '\n';
    return 1;
  }

  std::cout << "projection="
            << veritas::core::ToString(query->projection_id()) << '\n';
  for (const auto& callee : *callees) {
    std::cout << veritas::core::ToString(callee.node_id) << '\t'
              << callee.label << '\n';
  }
  return 0;
}
```

`OpenCurrent` resolves exactly one `(revision, build variant)` binding and
then loads its immutable projection. If a tool needs historical repeatability,
record `query->projection_id()` and reopen it later with `OpenProjection`.

Available native graph operations are:

```text
GetCallees(function)
GetCallers(function)
GetWriters(memory_object)
GetValueFlow(source_value, destination_value, budget)
GetCallPaths(source_function, destination_function, budget)
```

## 6. Handle bounded traversal correctly

Traversal is never an unqualified vector of paths:

```cpp
veritas::cpg::QueryBudget budget{
    .max_depth = 10,
    .max_nodes = 1000,
    .max_paths = 100,
};

auto flow = query->GetValueFlow(source_id, destination_id, budget);
if (!flow.ok()) {
  std::cerr << flow.status().message() << '\n';
  return 1;
}

if (!flow->truncation_reasons.empty()) {
  // Present a partial result. Do not conclude that omitted paths do not exist.
}
```

VERITAS distinguishes:

```text
complete, non-empty
complete, empty
truncated, non-empty
truncated, empty
```

An exact budget boundary is complete when the engine proves no additional
matching result exists. A consumer must use `truncation_reasons`, not infer
completeness from a count.

## 7. Compute semantic impact

The implemented building blocks are:

```cpp
auto old_artifact = repository->GetSummaryArtifact(old_id);
auto new_artifact = repository->GetSummaryArtifact(new_id);

// DiffSummaries currently accepts the concrete v1 message. A new
// version-neutral tool should first add a SummaryArtifact overload rather than
// down-cast or reinterpret v2 bytes.
```

The existing `veritas-diff` demonstrates the v1 path:

```text
GetSummary(old_id) + GetSummary(new_id)
  -> DiffSummaries
  -> DependencyIndex::GetImpactSet(delta, budget)
  -> changed components + affected consumers + truncation
```

For current v2 databases, implement and test a version-neutral
`DiffSummaryArtifacts` boundary before extending the CLI. Its behavior should:

1. require the same `FunctionVariantID` for old and new objects;
2. compare component kinds through version-neutral digest accessors;
3. classify semantic and evidence-only changes independently;
4. preserve added/removed component behavior across schema versions; and
5. feed the existing `DependencyIndex` without fabricating v1 objects.

This is a useful first contribution for a developer building richer analysis
tools over the current summary.v2 pipeline.

## 8. Turn the reader into a focused analysis

A SummaryDB tool should answer one semantic question. Examples that fit the
current APIs include:

- list functions whose `UNKNOWN` component is non-empty;
- enumerate direct and possible callees for one stable function;
- find writers of one stable memory object;
- compare the `RANGE_FACTS` component across two summary versions;
- calculate the bounded reverse impact of a component change; or
- export a small, explicitly budgeted native call/value-flow slice.

A tool should return:

```text
snapshot identity
semantic members
support/provenance references already present in summaries/CPG edges
component digests
completeness/truncation
diagnostics
```

It should not expose raw RocksDB bytes, accept arbitrary SQL, copy the entire
CPG into memory without a budget, or turn an empty partial result into a
negative assertion.

## 9. Test the tool

Use an isolated fixture under `tests/fixtures/projects/` and assert typed
behavior before golden output. A useful test set includes:

1. current summary.v2 listing succeeds;
2. historical v1 and current v2 coexist;
3. invalid revision/build/function ID kinds fail;
4. a query cannot mix endpoints from another projection;
5. exact-budget traversal is complete;
6. one-more-than-budget traversal reports the stable truncation reason;
7. insertion order does not change output ordering; and
8. missing or corrupt summary objects fail without falling back to another
   schema.

Run the focused test, then the full suite:

```bash
cmake --build --preset default
ctest --test-dir build -R '<focused-test-name>' --output-on-failure
ctest --preset default
git diff --check
```

For richer semantic queries and Evidence generation, continue with the
[developer guide](summarydb-evidence-ir-developer-guide.md) and the
[Agent review tutorial](tutorial-agent-code-review.md).
