# M6 LLVM-Native Thin VERITAS CPG Projection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and publish a deterministic VERITAS-owned thin CPG directly from the live linked LLVM `ProgramIr` and completed M5 mapped summaries, without any external CPG generator or serialized IR interchange.

**Architecture:** M6 adds a private projection stage after required M5 analysis and before `ProgramIr` destruction or current-binding publication. The stage borrows `ProgramIr` plus completed in-memory summaries, creates and validates a stable-ID `ThinCpg`, then a project publication coordinator advances summary and CPG current bindings in one SQLite transaction. Public query objects bind to immutable projection snapshots and return explicit traversal truncation metadata.

**Tech Stack:** C++20, CMake 3.23+, LLVM/Clang 22.x private APIs, pinned in-process SVF facts through VERITAS Summary IR, Protobuf, SQLite, GoogleTest.

**Spec:** `docs/specs/milestones/m06-thin-cpg-projection-design-spec.md`

## Global Constraints

- M6 consumes only a borrowed live `ProgramIr` and completed in-memory `FunctionSummary` objects; it accepts no `.bc`, `.ll`, LLVM-module pathname, serialized CPG, Joern export, PhASAR result, or subprocess output.
- No Joern or PhASAR CLI, service, schema generator, database, exporter, or standalone analysis process is introduced.
- Installed VERITAS headers expose no LLVM, SVF, Joern, or PhASAR native types.
- Persistent node kinds are limited to `Function`, `Parameter`, `Global`, `CallSite`, `MemoryObject`, `BasicBlockSummary`, `Summary`, and `Unknown` in V1.
- LLVM pointers, SVF pointers/node IDs, third-party IDs, every instruction, every AST expression, and temporary SSA values are never persisted.
- Alias facts use one `ALIASES` edge kind with an exact `MustAlias`, `MayAlias`, `NoAlias`, or `UnknownAlias` state; only M5-evaluated candidate pairs are emitted.
- M6 does not implement a second pointer analysis. A PhASAR-inspired clean-room refinement requires a separate design and runs as a versioned M5 stage before summary completion.
- `ProjectionID`, node IDs, and edge IDs use M2 canonical hashing; pointer addresses, allocation order, hash-table order, and thread scheduling never affect identity.
- Every persistent node kind has an explicit origin mapping: Function to `FunctionVariantID`, Parameter to mapped `ValueRef`, Global/MemoryObject to mapped `MemoryRef`, CallSite to `CallSiteID`, BasicBlockSummary to mapped `BasicBlockSummaryID`, Summary to `FunctionSummaryID`, and Unknown to a canonical scoped hash.
- Current summary bindings and the current CPG binding advance in one SQLite transaction after in-memory graph validation.
- Query objects are pinned to one immutable `ProjectionID`; traversal results distinguish no path from budget truncation.

---

### Task 1: Stable CPG Schema, Canonical Identity, and In-Memory Graph

**Files:**
- Create: `proto/veritas/cpg/v1/cpg.proto`
- Create: `include/veritas/cpg/CpgTypes.h`
- Create: `include/veritas/cpg/ThinCpg.h`
- Create: `src/cpg/CpgCanonicalizer.h`
- Create: `src/cpg/CpgCanonicalizer.cpp`
- Create: `src/cpg/ThinCpg.cpp`
- Test: `tests/unit/cpg/ThinCpgTest.cpp`
- Test: `tests/unit/cpg/CpgCanonicalizerTest.cpp`

**Interfaces:**
- Consumes: M2 `core::StableId` and versioned canonical writer
- Produces: `cpg::CpgNode`, `cpg::CpgEdge`, `cpg::SupportRef`, and `cpg::ProjectionMetadata`
- Produces: `cpg::ThinCpg::AddNode`, `AddEdge`, and `Validate`
- Produces: `cpg::CpgCanonicalizer::ProjectionId` and `CanonicalBytes`

- [ ] **Step 1: Write failing schema and graph invariant tests**

```cpp
TEST(ThinCpgTest, PreservesAllFourAliasStatesWithoutPairFanout) {
  ThinCpg graph = GraphWithMemoryNodes("mem:a", "mem:b");
  for (AliasState state : {AliasState::kMustAlias,
                           AliasState::kMayAlias,
                           AliasState::kNoAlias,
                           AliasState::kUnknownAlias}) {
    ASSERT_OK(graph.AddEdge(AliasEdge("mem:a", "mem:b", state,
                                      Support("summary:one", "prov:one"))));
  }
  EXPECT_EQ(graph.EdgesOfKind(EdgeKind::kAliases).size(), 4u);
}

TEST(ThinCpgTest, RejectsStableIdCollisionWithDifferentContent) {
  ThinCpg graph;
  ASSERT_OK(graph.AddNode(FunctionNode("funcvar:same", "first")));
  Status status = graph.AddNode(FunctionNode("funcvar:same", "second"));
  EXPECT_EQ(status.code(), StatusCode::FailedPrecondition);
}

TEST(CpgCanonicalizerTest, IgnoresInsertionOrderAndNativeAddresses) {
  ThinCpg forward = BuildFixtureGraph(InsertionOrder::kForward);
  ThinCpg reverse = BuildFixtureGraph(InsertionOrder::kReverse);
  EXPECT_EQ(CpgCanonicalizer::CanonicalBytes(forward),
            CpgCanonicalizer::CanonicalBytes(reverse));
  EXPECT_EQ(CpgCanonicalizer::ProjectionId(forward),
            CpgCanonicalizer::ProjectionId(reverse));
}
```

- [ ] **Step 2: Run the focused tests and verify failure**

Run:

```bash
cmake -S . -B build -DVERITAS_BUILD_TESTS=ON -DLLVM_PROJECT_BUILD_DIR="${LLVM_PROJECT_BUILD_DIR}"
cmake --build build --target ThinCpgTest CpgCanonicalizerTest
ctest --test-dir build -R "ThinCpg|CpgCanonicalizer" --output-on-failure
```

Expected: compilation fails because the CPG schema and graph types do not exist.

- [ ] **Step 3: Define the Protobuf and public VERITAS-only types**

Define only stable VERITAS data in the installed headers:

```cpp
namespace veritas::cpg {
enum class NodeKind {
  kFunction,
  kParameter,
  kGlobal,
  kCallSite,
  kMemoryObject,
  kBasicBlockSummary,
  kSummary,
  kUnknown,
};

enum class EdgeKind {
  kContains,
  kDeclares,
  kCalls,
  kMayCall,
  kReads,
  kWrites,
  kFlowsTo,
  kAliases,
  kDominatesSummary,
  kSummarizedBy,
  kUnknownAt,
};

enum class AliasState {
  kMustAlias,
  kMayAlias,
  kNoAlias,
  kUnknownAlias,
};

struct SupportRef {
  core::StableId function_summary_id;
  summary::ProvenanceRef provenance_ref;
  auto operator<=>(const SupportRef&) const = default;
};

struct CpgEdge {
  core::StableId edge_id;
  EdgeKind kind;
  core::StableId source_node_id;
  core::StableId target_node_id;
  std::optional<AliasState> alias_state;
  bool expandable;
  std::vector<SupportRef> support;
};
}
```

Mirror these fields in `cpg.proto`. Do not add native-pointer, third-party-node-ID, instruction-node, or M9 `FactID` fields.

- [ ] **Step 4: Implement deterministic insertion and validation**

`AddNode` and `AddEdge` must be idempotent only when canonical content matches. `Validate` checks endpoint existence, revision/build ownership, alias-state presence only on `kAliases`, sorted/deduplicated support records, expandable summary edges, and the V1 node-kind allowlist.

```cpp
Status ThinCpg::AddEdge(CpgEdge edge) {
  CanonicalizeSupport(&edge.support);
  if (edge.kind == EdgeKind::kAliases && !edge.alias_state.has_value()) {
    return Status::InvalidArgument("ALIASES edge requires alias_state");
  }
  if (edge.kind != EdgeKind::kAliases && edge.alias_state.has_value()) {
    return Status::InvalidArgument("alias_state is valid only for ALIASES");
  }
  return InsertOrVerifySame(&edges_, edge.edge_id, std::move(edge));
}
```

- [ ] **Step 5: Implement canonical graph bytes and `ProjectionID`**

Use M2's length-prefixed canonical writer with this fixed order:

```text
schema version "veritas.cpg.v1"
revision_id
build_variant_id
module_hash
sorted FunctionSummaryIDs
sorted canonical nodes
sorted canonical edges including support records
```

Hash those bytes with the M2 stable-ID builder using the `cpgproj` kind prefix. Never hash Protobuf map iteration or SQLite-file bytes.

- [ ] **Step 6: Run the unit tests and verify success**

Run: `ctest --test-dir build -R "ThinCpg|CpgCanonicalizer" --output-on-failure`

Expected: graph invariants, four-state alias representation, collision rejection, and deterministic identity tests pass.

- [ ] **Step 7: Commit the core graph model**

```bash
git add proto/veritas/cpg/v1 include/veritas/cpg src/cpg tests/unit/cpg
git commit -m "feat: add stable thin CPG model"
```

---

### Task 2: Private Projection Stage over Live `ProgramIr` and Completed Summaries

**Files:**
- Modify: `proto/veritas/summary/v1/summary.proto`
- Modify: `src/analysis/llvm/LocalFactExtractor.h`
- Modify: `src/analysis/llvm/LocalFactExtractor.cpp`
- Modify: `tests/integration/analysis/LocalFactExtractorTest.cpp`
- Create: `src/analysis/cpg/CpgProjectionStage.h`
- Create: `src/analysis/cpg/CpgProjectionStage.cpp`
- Create: `tests/integration/analysis/cpg/CpgProjectionStageTest.cpp`
- Reuse: `tests/fixtures/projects/multiple_tus_flow`
- Reuse: `tests/fixtures/projects/function_pointer`
- Reuse: `tests/fixtures/projects/field_access`

**Interfaces:**
- Consumes: borrowed `analysis::pipeline::ProgramIr`, completed in-memory `FunctionSummary` span, revision ID, and build-variant ID
- Produces: M4 `BasicBlockSummaryRef` values with canonical `BasicBlockSummaryID`
- Produces: `StatusOr<cpg::ThinCpg> analysis::cpg::BuildThinCpg(const CpgProjectionInput&)`
- Does not consume: `SvfSessionView`, SummaryDB readback, bitcode paths, or external-tool output

- [ ] **Step 1: Write failing live-input and mapping tests**

```cpp
TEST(CpgProjectionStageTest, BuildsFromLiveProgramIrAndCompletedSummaries) {
  ASSERT_OK_AND_ASSIGN(auto local, RunFixtureLocalAnalysis("multiple_tus_flow"));
  ASSERT_OK_AND_ASSIGN(auto completed,
                       RunRequiredSvfAndCompleteSummaries(local));
  ASSERT_OK_AND_ASSIGN(auto graph, analysis::cpg::BuildThinCpg({
      .program_ir = completed.program_ir,
      .completed_summaries = completed.summaries,
      .revision_id = completed.revision_id,
      .build_variant_id = completed.build_variant_id,
  }));
  EXPECT_TRUE(graph.HasNodeKind(cpg::NodeKind::kFunction));
  EXPECT_TRUE(graph.HasEdgeKind(cpg::EdgeKind::kFlowsTo));
  EXPECT_FALSE(graph.HasInstructionNodes());
}

TEST(CpgProjectionStageTest, PreservesMappedAliasStatesExactly) {
  auto graph = BuildProjectionFromSyntheticAliasSummaries({
      AliasFact("a", "b", AliasResult::MustAlias),
      AliasFact("b", "c", AliasResult::MayAlias),
      AliasFact("c", "d", AliasResult::NoAlias),
      AliasFact("d", "e", AliasResult::UnknownAlias),
  });
  EXPECT_EQ(AliasStates(graph),
            ElementsAre(AliasState::kMustAlias, AliasState::kMayAlias,
                        AliasState::kNoAlias, AliasState::kUnknownAlias));
}

TEST(CpgProjectionStageTest, DoesNotInventSourceSemanticNodes) {
  auto graph = BuildFixtureProjection("field_access");
  EXPECT_FALSE(graph.HasNodeKindName("Type"));
  EXPECT_FALSE(graph.HasNodeKindName("Field"));
  EXPECT_TRUE(graph.HasMemoryObjectWithFieldPath("record.payload"));
}

TEST(CpgProjectionStageTest, EveryPersistentNodeUsesMappedStableIdentity) {
  auto graph = BuildFixtureProjection("multiple_tus_flow");
  EXPECT_ALL_FUNCTION_IDS_ARE_FUNCTION_VARIANT_IDS(graph);
  EXPECT_ALL_PARAMETER_IDS_ARE_MAPPED_VALUE_REFS(graph);
  EXPECT_ALL_GLOBAL_IDS_ARE_MAPPED_MEMORY_REFS(graph);
  EXPECT_ALL_CALLSITE_IDS_ARE_CALLSITE_IDS(graph);
  EXPECT_ALL_BLOCK_IDS_ARE_BASIC_BLOCK_SUMMARY_IDS(graph);
}
```

- [ ] **Step 2: Run the focused tests and verify failure**

Run: `ctest --test-dir build -R CpgProjectionStage --output-on-failure`

Expected: compilation fails because `CpgProjectionStage` is missing.

- [ ] **Step 3: Define the private borrowed input boundary**

```cpp
namespace veritas::analysis::cpg {
struct CpgProjectionInput {
  const pipeline::ProgramIr& program_ir;
  std::span<const summary::v1::FunctionSummary> completed_summaries;
  core::StableId revision_id;
  core::StableId build_variant_id;
};

StatusOr<::veritas::cpg::ThinCpg> BuildThinCpg(
    const CpgProjectionInput& input);
}
```

Keep this header under `src/analysis/cpg`; public installed headers must not include `ProgramIr` or LLVM types.

- [ ] **Step 4: Add stable M4 basic-block summary references**

Add `BasicBlockSummaryRef` to Summary IR dominator/local-CFG facts. M4 computes its ID with the M2 canonical writer from the owning `FunctionVariantID`, ordered mapped semantic `SourceAnchorID` members, and sorted mapped predecessor/successor anchor IDs. Emit no block reference when those mappings are insufficient; emit the existing scoped M4 unknown instead.

```cpp
BasicBlockSummaryRef BuildBasicBlockSummaryRef(
    const llvm::BasicBlock& block, const OriginMap& origins,
    core::StableId function_variant_id) {
  CanonicalWriter writer("bbsummary.v1");
  writer.Add(function_variant_id);
  writer.AddRange(OrderedMappedSemanticAnchors(block, origins));
  writer.AddSortedRange(MappedPredecessorSuccessorAnchors(block, origins));
  return BasicBlockSummaryRef{BuildStableId("bbsummary", writer.bytes())};
}
```

- [ ] **Step 5: Project stable LLVM structure while `ProgramIr` is alive**

Collect functions, parameters, globals, callsites, basic-block summaries, and resolvable memory objects. Resolve every native value through `OriginMap` before creating a node. Sort by the resolved stable ID before insertion.

```cpp
for (const llvm::Function* function : SortedDefinedFunctions(module)) {
  VERITAS_ASSIGN_OR_RETURN(core::StableId function_id,
      input.program_ir.origin_map().FunctionVariantId(*function));
  VERITAS_RETURN_IF_ERROR(graph.AddNode(
      MakeFunctionNode(function_id, SourceProperties(*function, origin_map))));
  ProjectParametersAndCalls(*function, input, &graph);
}
```

Use this complete mapping table:

```text
Function          -> OriginMap FunctionVariantID
Parameter         -> OriginMap ValueRef for llvm::Argument
Global            -> OriginMap MemoryRef for llvm::GlobalValue
CallSite          -> completed CallFact CallSiteID, cross-checked with origin map
MemoryObject      -> OriginMap/completed-fact MemoryRef
BasicBlockSummary -> mapped M4 BasicBlockSummaryID
Summary           -> FunctionSummaryID
Unknown           -> canonical revision/build/scope/kind/provenance hash
```

When origin resolution fails, emit a stable `Unknown` node and `UNKNOWN_AT` edge scoped by revision, build, owning function, unknown kind, and mapped provenance. Never derive identity from an address, LLVM iteration index, parameter ordinal alone, or basic-block order.

- [ ] **Step 6: Project completed summary facts into semantic edges**

Map call, memory-effect, value-flow, dominator, summary, unknown, and alias components in sorted summary-ID order. Preserve fact epistemic state and attach the exact `FunctionSummaryID` plus opaque `summary::ProvenanceRef` support record.

```cpp
for (const auto& fact : SortedAliasFacts(summary)) {
  VERITAS_RETURN_IF_ERROR(graph.AddEdge(MakeAliasEdge(
      ResolveMemoryNode(fact.left()), ResolveMemoryNode(fact.right()),
      ToCpgAliasState(fact.result()),
      SupportRef{summary_id, fact.provenance_ref()})));
}
```

Do not run alias analysis in this stage. Do not create edges for pointer pairs absent from the completed summaries.

- [ ] **Step 7: Canonicalize and validate before returning**

Set `module_hash`, sorted completed summary IDs, revision ID, and build-variant ID in projection metadata. Run `ThinCpg::Validate` before returning. A validation error is fatal to project publication.

- [ ] **Step 8: Run projection tests and verify success**

Run: `ctest --test-dir build -R CpgProjectionStage --output-on-failure`

Expected: live-IR projection, exact alias-state mapping, source-property behavior, no-instruction persistence, and unknown handling pass.

- [ ] **Step 9: Commit the live projection stage**

```bash
git add proto/veritas/summary/v1/summary.proto src/analysis/llvm/LocalFactExtractor.* src/analysis/cpg tests/integration/analysis/LocalFactExtractorTest.cpp tests/integration/analysis/cpg
git commit -m "feat: project CPG from live program analysis"
```

---

### Task 3: SQLite Projection Repository and Atomic Project Publication

**Files:**
- Modify: `src/summarydb/schema/v1.sql`
- Modify: `include/veritas/summarydb/SummaryRepository.h`
- Modify: `src/summarydb/SummaryRepository.cpp`
- Create: `include/veritas/cpg/CpgRepository.h`
- Create: `src/cpg/CpgRepository.cpp`
- Create: `include/veritas/summarydb/ProjectPublicationCoordinator.h`
- Create: `src/summarydb/ProjectPublicationCoordinator.cpp`
- Modify: `include/veritas/analysis/ProjectAnalyzer.h`
- Modify: `src/analysis/ProjectAnalyzer.cpp`
- Test: `tests/unit/cpg/CpgRepositoryTest.cpp`
- Test: `tests/integration/summarydb/ProjectPublicationCoordinatorTest.cpp`

**Interfaces:**
- Produces: `cpg::CpgRepository::StageProjection(metadata::Transaction&, const ThinCpg&)`
- Produces: `summarydb::SummaryRepository::StageCurrentBindings(metadata::Transaction&, span<CompletedSummary>)`
- Produces: `summarydb::ProjectPublicationCoordinator::Publish(CompletedProjectAnalysis)`
- Changes: `ProjectAnalyzer` builds the CPG before handing completed summaries and graph to the coordinator

- [ ] **Step 1: Write failing repository and atomic-failure tests**

```cpp
TEST(CpgRepositoryTest, OpensHistoricalProjectionById) {
  auto repository = MakeCpgRepository();
  ThinCpg old_graph = FixtureGraph("revision:old");
  ThinCpg new_graph = FixtureGraph("revision:new");
  ASSERT_OK(PublishProjection(repository, old_graph));
  ASSERT_OK(PublishProjection(repository, new_graph));
  ASSERT_OK_AND_ASSIGN(auto loaded,
                       repository.LoadProjection(old_graph.projection_id()));
  EXPECT_EQ(CpgCanonicalizer::CanonicalBytes(loaded),
            CpgCanonicalizer::CanonicalBytes(old_graph));
}

TEST(ProjectPublicationCoordinatorTest, ProjectionFailureAdvancesNoBindings) {
  auto fixture = PublishedProjectFixture();
  auto before = fixture.CurrentBindings();
  fixture.InjectFailure(PublicationFailurePoint::kAfterCpgRowsBeforeCommit);
  Status status = fixture.Publish(CompletedAnalysisWithNewSummaryAndGraph());
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(fixture.CurrentBindings(), before);
}

TEST(ProjectPublicationCoordinatorTest, RejectsMismatchedGraphSnapshot) {
  auto fixture = PublishedProjectFixture();
  auto completed = CompletedAnalysisWithNewSummaryAndGraph();
  completed.graph.mutable_metadata().summary_ids.pop_back();
  Status status = fixture.Publish(std::move(completed));
  EXPECT_EQ(status.code(), StatusCode::FailedPrecondition);
  EXPECT_EQ(fixture.CurrentBindings(), fixture.OriginalBindings());
}
```

- [ ] **Step 2: Run the focused tests and verify failure**

Run: `ctest --test-dir build -R "CpgRepository|ProjectPublicationCoordinator" --output-on-failure`

Expected: compilation fails because repository staging and the coordinator are missing.

- [ ] **Step 3: Add projection and adjacency tables**

Add these logical tables and foreign keys to `schema/v1.sql`:

```sql
CREATE TABLE cpg_projections (
  projection_id TEXT PRIMARY KEY,
  schema_version TEXT NOT NULL,
  revision_id TEXT NOT NULL,
  build_variant_id TEXT NOT NULL,
  module_hash TEXT NOT NULL,
  canonical_hash TEXT NOT NULL
);

CREATE TABLE cpg_nodes (
  projection_id TEXT NOT NULL,
  node_id TEXT NOT NULL,
  node_kind INTEGER NOT NULL,
  canonical_bytes BLOB NOT NULL,
  PRIMARY KEY (projection_id, node_id),
  FOREIGN KEY (projection_id) REFERENCES cpg_projections(projection_id)
);

CREATE TABLE cpg_edges (
  projection_id TEXT NOT NULL,
  edge_id TEXT NOT NULL,
  edge_kind INTEGER NOT NULL,
  source_node_id TEXT NOT NULL,
  target_node_id TEXT NOT NULL,
  canonical_bytes BLOB NOT NULL,
  PRIMARY KEY (projection_id, edge_id),
  FOREIGN KEY (projection_id) REFERENCES cpg_projections(projection_id)
);

CREATE TABLE current_cpg_projections (
  revision_id TEXT NOT NULL,
  build_variant_id TEXT NOT NULL,
  projection_id TEXT NOT NULL,
  PRIMARY KEY (revision_id, build_variant_id)
);
```

Create indexes for `(projection_id, source_node_id, edge_kind)`, `(projection_id, target_node_id, edge_kind)`, callsites by owning function, memory readers/writers, and flow endpoints.

- [ ] **Step 4: Implement transaction-scoped staging APIs**

Neither repository staging method may begin or commit its own transaction.

```cpp
Status CpgRepository::StageProjection(metadata::Transaction& transaction,
                                      const ThinCpg& graph) {
  VERITAS_RETURN_IF_ERROR(graph.Validate());
  VERITAS_RETURN_IF_ERROR(InsertProjection(transaction, graph.metadata()));
  VERITAS_RETURN_IF_ERROR(InsertNodes(transaction, graph.nodes()));
  VERITAS_RETURN_IF_ERROR(InsertEdges(transaction, graph.edges()));
  return ReplaceCurrentProjection(transaction, graph.metadata());
}
```

`SummaryRepository::StageCurrentBindings` inserts summary metadata and replaces current bindings using the same `metadata::Transaction` supplied by the coordinator.

- [ ] **Step 5: Implement the publication coordinator**

Write immutable summary objects before the metadata transaction, then stage summary metadata/current bindings and the validated graph inside one SQLite transaction:

```cpp
Status ProjectPublicationCoordinator::Publish(
    CompletedProjectAnalysis completed) {
  VERITAS_RETURN_IF_ERROR(completed.graph.Validate());
  VERITAS_RETURN_IF_ERROR(ValidateSnapshotCorrespondence(
      completed.graph.metadata(), completed.summaries,
      completed.revision_id, completed.build_variant_id,
      completed.module_hash));
  VERITAS_RETURN_IF_ERROR(
      summaries_.PutImmutableObjects(completed.summaries));
  VERITAS_ASSIGN_OR_RETURN(auto transaction, metadata_.BeginTransaction());
  VERITAS_RETURN_IF_ERROR(
      summaries_.StageCurrentBindings(transaction, completed.summaries));
  VERITAS_RETURN_IF_ERROR(
      cpg_.StageProjection(transaction, completed.graph));
  return transaction.Commit();
}
```

`ValidateSnapshotCorrespondence` compares revision ID, build-variant ID, module hash, and exact canonical sorted `FunctionSummaryID` set. It runs before immutable object writes or transaction creation. Failure advances no bindings and writes no object. A later transaction failure may leave immutable unbound objects eligible for reuse, but still advances neither current binding.

- [ ] **Step 6: Place M6 before publication in `ProjectAnalyzer`**

Refactor the post-M5 path so it retains the live `ProgramIr` and completed summaries, calls `BuildThinCpg`, then calls the coordinator. Do not read current summaries back from SummaryDB.

```cpp
VERITAS_ASSIGN_OR_RETURN(auto completed,
    RunRequiredSvfAnalysis(std::move(local), run_context, config.svf));
VERITAS_ASSIGN_OR_RETURN(auto graph, analysis::cpg::BuildThinCpg({
    .program_ir = completed.program_ir,
    .completed_summaries = completed.summaries,
    .revision_id = completed.revision_id,
    .build_variant_id = completed.build_variant_id,
}));
VERITAS_RETURN_IF_ERROR(publication_.Publish(
    CompletedProjectAnalysis{std::move(completed.summaries),
                             std::move(graph)}));
```

- [ ] **Step 7: Run repository, failure-injection, and ProjectAnalyzer tests**

Run:

```bash
ctest --test-dir build -R "CpgRepository|ProjectPublicationCoordinator|ProjectAnalyzerSvf" --output-on-failure
```

Expected: current bindings advance together, failure preserves the previous snapshot, historical projections load by ID, and existing required-SVF behavior remains intact.

- [ ] **Step 8: Commit atomic project publication**

```bash
git add src/summarydb/schema/v1.sql include/veritas/summarydb src/summarydb include/veritas/cpg/CpgRepository.h src/cpg/CpgRepository.cpp include/veritas/analysis/ProjectAnalyzer.h src/analysis/ProjectAnalyzer.cpp tests/unit/cpg tests/integration/summarydb
git commit -m "feat: publish summaries and CPG atomically"
```

---

### Task 4: Snapshot-Bound Query API, Budgeted Traversal, and CLI

**Files:**
- Create: `include/veritas/cpg/CpgQuery.h`
- Create: `src/cpg/CpgQuery.cpp`
- Test: `tests/unit/cpg/CpgQueryTest.cpp`
- Modify: `src/tools/veritas-query.cpp`
- Test: `tests/integration/cpg/VeritasQueryCpgTest.cpp`

**Interfaces:**
- Produces: `CpgQuery::OpenProjection` and `OpenCurrent`
- Produces: `TraversalResult<CpgPath>` with all truncation reasons and explored counts
- Produces: caller, callee, writer, value-flow, and call-path queries over one immutable `ProjectionID`

- [ ] **Step 1: Write failing snapshot and truncation tests**

```cpp
TEST(CpgQueryTest, DistinguishesNoPathFromTruncatedSearch) {
  CpgQuery query = OpenFixtureProjection("long_flow");
  ASSERT_OK_AND_ASSIGN(auto no_path,
      query.GetValueFlow(Id("unconnected:a"), Id("unconnected:b"),
                         QueryBudget{10, 100, 10}));
  EXPECT_TRUE(no_path.items.empty());
  EXPECT_TRUE(no_path.truncation_reasons.empty());

  ASSERT_OK_AND_ASSIGN(auto truncated,
      query.GetValueFlow(Id("flow:start"), Id("flow:end"),
                         QueryBudget{2, 3, 1}));
  EXPECT_THAT(truncated.truncation_reasons,
              Contains(TruncationReason::kMaxDepth));
}

TEST(CpgQueryTest, CurrentQueryPinsResolvedProjection) {
  auto repository = RepositoryWithProjection("cpgproj:old");
  ASSERT_OK_AND_ASSIGN(auto query,
      CpgQuery::OpenCurrent(repository, Id("rev:one"), Id("build:one")));
  PublishNewCurrentProjection(repository, "cpgproj:new");
  EXPECT_EQ(query.projection_id(), Id("cpgproj:old"));
}

TEST(CpgQueryTest, ExactBudgetBoundaryIsComplete) {
  CpgQuery query = OpenFixtureProjection("single_path_exact_budget");
  ASSERT_OK_AND_ASSIGN(auto result,
      query.GetValueFlow(Id("flow:start"), Id("flow:end"),
                         QueryBudget{.max_depth = 3,
                                     .max_nodes = 4,
                                     .max_paths = 1}));
  EXPECT_EQ(result.items.size(), 1u);
  EXPECT_TRUE(result.truncation_reasons.empty());
  EXPECT_EQ(result.explored_nodes, 4u);
  EXPECT_EQ(result.explored_paths, 1u);
}
```

- [ ] **Step 2: Run the focused query tests and verify failure**

Run: `ctest --test-dir build -R CpgQuery --output-on-failure`

Expected: compilation fails because snapshot-bound query and traversal result types are missing.

- [ ] **Step 3: Define query budgets and result metadata**

```cpp
enum class TruncationReason { kMaxDepth, kMaxNodes, kMaxPaths };

struct QueryBudget {
  std::size_t max_depth;
  std::size_t max_nodes;
  std::size_t max_paths;
};

template <typename T>
struct TraversalResult {
  std::vector<T> items;
  std::vector<TruncationReason> truncation_reasons;
  std::size_t explored_nodes;
  std::size_t explored_paths;
};
```

`Status` reports storage, schema, or invalid-input errors only. A non-empty `truncation_reasons` vector marks an otherwise successful but incomplete traversal.

- [ ] **Step 4: Implement immutable snapshot binding and adjacency queries**

`OpenCurrent` resolves `(revision_id, build_variant_id)` to `ProjectionID` once, then delegates to `OpenProjection`. Store only that immutable ID in `CpgQuery`. Load outgoing and incoming edges through the required SQLite indexes.

- [ ] **Step 5: Implement deterministic budgeted BFS**

Visit adjacent edges in `(edge_kind, target_node_id, edge_id)` order. Stop adding work when any budget is exhausted, record every exhausted reason, and retain already-completed paths.

```cpp
for (const Candidate& candidate : eligible_candidates) {
  if (candidate.depth > budget.max_depth) {
    reasons.insert(kMaxDepth);
    continue;
  }
  if (WouldAddNewNode(candidate) && explored_nodes >= budget.max_nodes) {
    reasons.insert(kMaxNodes);
    continue;
  }
  if (candidate.completes_path && result.items.size() >= budget.max_paths) {
    reasons.insert(kMaxPaths);
    continue;
  }
  Accept(candidate);
}
```

Record a truncation reason only when an otherwise eligible node or completed path is rejected. Merely reaching a limit at the end of a complete search is not truncation. Add one-over-limit fixtures for each reason and exact-boundary fixtures that remain untruncated. Unknown call nodes are terminal for call-path traversal; never expand them to all functions.

- [ ] **Step 6: Add CLI commands with explicit snapshot and budget output**

Support:

```text
veritas-query callees <function-id> --revision <id> --build <id>
veritas-query flow <src-id> <dst-id> --projection <id> --max-depth <n> --max-nodes <n> --max-paths <n>
```

The flow command prints `Projection`, `Paths`, `Explored nodes`, `Explored paths`, and `Truncated by`. It exits successfully for a truncated result but emits a machine-readable `incomplete=true` field in JSON output.

- [ ] **Step 7: Run unit and CLI tests**

Run: `ctest --test-dir build -R "CpgQuery|VeritasQueryCpg" --output-on-failure`

Expected: snapshot pinning, no-path distinction, all three budget reasons, exact-boundary completion, deterministic path order, unknown-call termination, and CLI output tests pass.

- [ ] **Step 8: Commit CPG queries**

```bash
git add include/veritas/cpg/CpgQuery.h src/cpg/CpgQuery.cpp src/tools/veritas-query.cpp tests/unit/cpg/CpgQueryTest.cpp tests/integration/cpg/VeritasQueryCpgTest.cpp
git commit -m "feat: query immutable CPG projections"
```

---

### Task 5: End-to-End Ownership, Determinism, and Boundary Verification

**Files:**
- Modify: `src/tools/veritas-build.cpp`
- Create: `tests/integration/analysis/cpg/ProjectAnalyzerCpgTest.cpp`
- Create: `tests/integration/analysis/cpg/RepeatedCpgProjectionTest.cpp`
- Create: `tests/integration/analysis/cpg/RequiredCpgBoundaryTest.cpp`
- Modify: `docs/plans/veritas-backbone-milestone-roadmap.md`

**Interfaces:**
- Consumes: the standard `veritas-build analyze --project` pipeline
- Produces: `projection_id`, graph node/edge counts, and publication status in diagnostic output
- Produces: regression protection for in-process ownership and prohibited external/artifact paths

- [ ] **Step 1: Write failing end-to-end and deterministic-output tests**

```cpp
TEST(ProjectAnalyzerCpgTest, StandardAnalysisPublishesSummariesAndCpg) {
  auto fixture = MakeIntegrationAnalyzerFixture();
  ASSERT_OK_AND_ASSIGN(auto result, fixture.analyzer.AnalyzeProject(
      ProjectRequest("multiple_tus_flow"), AnalysisConfig::Default()));
  EXPECT_FALSE(result.published_summary_ids.empty());
  EXPECT_FALSE(result.projection_id.empty());
  ASSERT_OK_AND_ASSIGN(auto graph,
      fixture.cpg_repository.LoadProjection(result.projection_id));
  EXPECT_TRUE(graph.HasEdgeKind(cpg::EdgeKind::kFlowsTo));
}

TEST(RepeatedCpgProjectionTest, IdenticalInputsProduceCanonicalEquality) {
  auto first = AnalyzeFixtureAndLoadCpg("multiple_tus_flow");
  auto second = AnalyzeFixtureAndLoadCpg("multiple_tus_flow");
  EXPECT_EQ(first.projection_id(), second.projection_id());
  EXPECT_EQ(CpgCanonicalizer::CanonicalBytes(first),
            CpgCanonicalizer::CanonicalBytes(second));
}
```

- [ ] **Step 2: Run the focused end-to-end tests and verify failure**

Run: `ctest --test-dir build -R "ProjectAnalyzerCpg|RepeatedCpgProjection|RequiredCpgBoundary" --output-on-failure`

Expected: tests fail until the full pipeline reports and verifies the projection.

- [ ] **Step 3: Report projection completion from the standard CLI**

Extend `ProjectAnalysisResult` and `veritas-build` output with the stable projection ID and counts. Add no alternate CPG-input or LLVM-artifact command.

```text
Analysis complete
Published summaries: 17
CPG projection: cpgproj:sha256:...
CPG nodes: 142
CPG edges: 311
```

- [ ] **Step 4: Add public-header and external-tool boundary tests**

Implement source-tree assertions equivalent to:

```bash
if rg -n '#include <(llvm|SVF)/|llvm::|SVF::|Joern|PhASAR' include/veritas; then
  exit 1
fi
if rg -n 'joern|phasar|--(bitcode|llvm-module|cpg-input|joern-input|phasar-input)|system\(|popen\(' CMakeLists.txt cmake src include tests; then
  exit 1
fi
```

The scan intentionally excludes `docs/`, where architecture constraints name prohibited tools. It also asserts that CMake defines no Joern/PhASAR dependency or executable discovery.

- [ ] **Step 5: Verify graph scope is not instruction-proportional**

Analyze a fixture variant with extra arithmetic instructions but the same functions, callsites, memory objects, and mapped summary relations. Assert its global node count is unchanged and no persistent node reports an instruction kind.

- [ ] **Step 6: Align the backbone M6 summary**

Update the M6 section to state the live `ProgramIr` + completed-summary input, V1 node allowlist, generic four-state `ALIASES` relation, one-transaction publication, snapshot-bound query contract, and no Joern/PhASAR generator. Link this implementation plan for task-level details.

- [ ] **Step 7: Run the standard build and full test suite**

```bash
cmake -S . -B build -DVERITAS_BUILD_TESTS=ON -DLLVM_PROJECT_BUILD_DIR="${LLVM_PROJECT_BUILD_DIR}"
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: the standard in-process M4/M5/M6 pipeline, repository/query tests, repeated analyses, CLI tests, and boundary scans all pass.

- [ ] **Step 8: Commit end-to-end verification and documentation**

```bash
git add src/tools/veritas-build.cpp tests/integration/analysis/cpg docs/plans/veritas-backbone-milestone-roadmap.md
git commit -m "test: verify LLVM-native CPG pipeline"
```

---

## Milestone Verification

- [ ] Run the complete configured build and suite:

```bash
cmake -S . -B build -DVERITAS_BUILD_TESTS=ON -DLLVM_PROJECT_BUILD_DIR="${LLVM_PROJECT_BUILD_DIR}"
cmake --build build
ctest --test-dir build --output-on-failure
```

- [ ] Verify public/native and external-tool boundaries:

```bash
if rg -n '#include <(llvm|SVF)/|llvm::|SVF::|Joern|PhASAR' include/veritas; then
  exit 1
fi
if rg -n 'joern|phasar|--(bitcode|llvm-module|cpg-input|joern-input|phasar-input)|system\(|popen\(' CMakeLists.txt cmake src include tests; then
  exit 1
fi
```

- [ ] Verify the task branch is mechanically clean:

```bash
git diff --check
git status --short
```

- [ ] Review the final branch diff and confirm it contains only M6 projection, publication, query, tests, and directly related documentation changes.
