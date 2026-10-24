# VERITAS Architecture Review and Redesign Proposal

Technical assessment · 24 October 2026 · LLVM / SVF / Soufflé / SummaryDB / EvidenceIR / LLM Agents

## 1. Executive assessment

My recommendation is to evolve VERITAS from a whole-program static-analysis-first platform into an evidence-driven, LLM-guided program reasoning system, with selective formal verification.

The distinction is fundamental:

- Current philosophy: Analyze the entire program, construct a comprehensive semantic database, derive program facts, and finally provide a condensed representation to an LLM.
- Proposed philosophy: Build a lightweight structural understanding of the program, let an LLM formulate and investigate semantic hypotheses, and invoke increasingly powerful static-analysis techniques only where they can resolve a specific question.

This should not mean abandoning LLVM, SVF, Datalog, or EvidenceIR. It means changing their architectural responsibilities.

The strongest existing VERITAS assets are its stable identity model, compositional function summaries, incremental-analysis concepts, provenance discipline, and explicit handling of uncertainty. These are worth preserving.

The weakest aspect is the requirement to construct and persist a substantial amount of low-level analysis state before knowing which program properties the user or Agent actually needs to investigate.

That decision creates three coupled problems:

1. Excessive computation and storage, including facts that will never be queried.
2. Expensive recursive whole-program analysis, even when the question concerns a small portion of the program.
3. An LLM relegated to interpreting conclusions rather than participating in semantic understanding, hypothesis formation, and analysis planning.

### The repository already demonstrates a structural scaling problem

The project's published scaling investigations report the following LevelDB benchmark characteristics:

Analyzed functions

# 3,548

WPA component executions

# 13,716

Published database rows

# 4.13M

Reported storage size

# 4–7 GiB

The storage figure is identified in the project's scaling roadmap as an observed estimate rather than an independently controlled disk measurement. The results concern an approximately 10 KLOC fixture, not a million-line application.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1

The existing roadmap explicitly acknowledges that constant-factor performance improvements cannot solve a problem requiring potentially two orders of magnitude better scaling.

This is important: the project is not merely suffering from inefficient database operations or insufficient Datalog optimization. Its current analysis granularity and materialization strategy deserve reconsideration.

## 2. Current implementation status

The repository's implementation is more advanced than its top-level README suggests, but still well short of an operational Agent-based program verification platform.

### 2.1 What is implemented versus planned

| Capability                          | Status               | Assessment                                                                               |
| ----------------------------------- | -------------------- | ---------------------------------------------------------------------------------------- |
| Project-level C/C++ ingestion       | Implemented          | `compile_commands.json`, Clang CodeGen, LLVM IR                                          |
| Function Summary IR                 | Implemented          | v2 summaries, stable identifiers, content-addressed storage                              |
| SVF analysis                        | Implemented          | In-process Andersen-style points-to analysis, SVFG and mapped facts                      |
| Thin CPG                            | Implemented          | Function-oriented projection with bounded graph queries                                  |
| Incremental infrastructure          | Partial              | Dependency structures and component caching exist; end-to-end invalidation is incomplete |
| Soufflé whole-program analysis      | Implemented          | Recursive domain analysis, SCC scheduling, fact derivation                               |
| SummaryDB persistence               | Implemented          | RocksDB + SQLite; fact and provenance storage                                            |
| EvidenceIR construction             | Implemented, limited | M10B/M10C query and serialization infrastructure                                         |
| Agent integration                   | Not delivered        | No complete production Review Agent or verification loop                                 |
| Selective static analysis           | Proposed             | No independent lightweight, demand-driven runtime                                        |
| External LLVM bitcode ingestion     | Pending              | M11 design exists                                                                        |
| Joern/external-provider integration | Pending              | M12 design exists                                                                        |

The distinction around M10B/M10C is especially important. The current source tree supports `EvidenceQueryService`, `EvidenceCaseBuilder`, EIR-T, JSON, and Protobuf representations. However, its end-to-end semantic coverage remains limited. The milestone matrix and parts of the README lag behind newer implementation guides.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+2



### 2.2 The real implemented pipeline

Project Ingestion

compile_commands.json → Clang → linked LLVM IR

Static Analysis

Local extraction + whole-program SVF

Function Summary IR + Thin CPG

Canonical IDs, local effects, calls, flows

SCC-based Soufflé WPA

Recursive facts, closures, witnesses

SummaryDB + Fact/Provenance Store

RocksDB, SQLite, immutable publication

Evidence Query + EvidenceCase

Bounded queries and typed EIR serialization

Review Agent + Verifier

Target architecture, not production-ready

This follows the actual orchestration in `ProjectAnalyzer.cpp`, including its four WPA component domains: reachability, memory effects, flow, and unknown effects.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1



### 2.3 Significant recent progress

The team has achieved meaningful engineering improvements:

- M8R/M9 added stronger deterministic execution, exact cache identities, validated provenance, and atomic fact publication.
- M10A expanded interprocedural parameter and return flow, along with recursive analysis domains.
- M10B/M10C delivered a working typed Evidence representation and serialization path, though not complete semantic producers.
- M13 introduced a baseline-versus-scaled compatibility mechanism and store-equivalence instrumentation.
- Multiple profiling/optimization rounds improved the measured LevelDB workload substantially.

The latest performance result I found, from merged PR #150 on October 1, reported approximately 391.46 seconds wall time and 390.86 seconds CPU time, improving on the earlier 421-second CPU baseline. It did not establish a memory reduction. These were Debug-build measurements, not independently repeated production benchmarks.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+2



Meanwhile, [PR #152](https://github.com/skg7on/VERITAS/pull/152?utm_source=chatgpt.com), opened on October 3 and still shown as open at review time, proposes a shift toward LLM-driven code review with selective analysis. This direction is consistent with my recommendation, but the PR explicitly changes documentation rather than the runtime.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+1



Assessment: VERITAS has a substantial deterministic-analysis backbone. Its next milestone should demonstrate that this backbone improves real code reasoning and defect detection, not simply extend the number of analysis relations.

## 3. Strengths worth preserving

### A. Compositional Function Summary IR

This is arguably VERITAS's most valuable long-term architectural choice.

A function summary can abstract externally visible behavior without exposing every LLVM instruction:

- Calls and interprocedural parameter/return flows
- Memory reads and writes
- Unknown external effects
- Value-flow relationships
- Potentially ranges, resource lifetimes, locks, and state transitions

Content-addressed summaries and component hashes provide a promising basis for both incremental verification and semantic retrieval.

However, the summary must evolve from an artifact optimized primarily for static-analysis propagation into an artifact that also explains what the function means to its callers.

### B. LLVM and SVF are authoritative structural foundations

Clang provides C/C++ language semantics that raw text retrieval cannot reliably reconstruct. LLVM offers canonical control/data relationships. SVF supports sophisticated pointer, alias, and value-flow analyses, including demand-driven techniques in its broader framework.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+1



These tools should remain available as evidence producers. Their outputs are particularly valuable when the LLM is uncertain about aliasing, indirect calls, memory effects, or interprocedural flow.

### C. EvidenceIR establishes a useful trust boundary

The distinction between `MUST`, `MAY`, `MUST_NOT`, `INFERRED`, `ASSUMED`, and `UNKNOWN` is excellent.

It establishes that LLM-generated explanations are not automatically program facts. A verified conclusion must depend on the authority, scope, assumptions, and soundness of the validating analysis.

The underlying concept is correct: EvidenceIR should capture a reviewable argument, not become another serialization of the entire program.

### D. Strong emphasis on determinism and provenance

Immutable facts, canonical IDs, witness-based explanations, cache validation, and differential conformance testing are unusually rigorous foundations for a project that will eventually integrate probabilistic reasoning.

The project can use these mechanisms to distinguish:

- What the compiler established
- What abstract interpretation conservatively approximated
- What a test reproduced
- What a solver proved under assumptions
- What the LLM merely inferred

These strengths make VERITAS more defensible than an LLM reviewer that simply reads source files and produces plausible explanations.

## 4. Critical weaknesses and root-cause analysis

I would rank the following six weaknesses by their impact on VERITAS's future viability.

### 4.1 Global transitive-closure materialization is fundamentally expensive

Architectural bottleneck

The clearest example is the current `GlobalFlow` relation in `logic/flow/global_flow.v2.dl`:

```
GlobalFlow(s, d, e) :-
    GlobalFlow(s, m, e1),
    GlobalFlow(m, d, e2),
    WeakenEpistemic(e1, e2, e).
```

The implementation computes transitive flows across the program and persists the resulting tuples and their witnesses.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



Consider a sequence of N functions:

```
F1 → F2 → F3 → ... → FN
```

The underlying call graph contains just N−1 edges.

But persisting all reachable function pairs creates:

\\[ N\_{\text{reachable pairs}}=\frac{N(N-1)}{2}=O(N^2) \\]

Illustrative growth in a linear call chain

Direct edgesReachability pairs

05.5K11K16.5K22K102550100200

Theoretical graph illustration, not measured VERITAS results

For a simple reachability query, we typically need to answer:

> Can function A reach function B?

We do not need to precompute and persist every possible answer.

The existing implementation also constructs verbose witness keys for recursive derivations. Its scaling roadmap documents significant string concatenation, serialization, and repeated hashing overhead.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



Recommendation: Store the base graph and compositional summaries. Generate reachability paths and data-flow witnesses on demand, within a bounded query scope.

For reachability, use graph traversal or SCC condensation. For interprocedural value flow, consider tabulation-based demand-driven techniques such as IFDS/IDE where their assumptions fit the analysis. For pointer-sensitive reasoning, retain SVF as an optional refinement provider.

An important qualification: LevelDB's shallow dependency structure does not establish that quadratic growth dominates real industrial workloads. The project's M14 synthetic chain/star/DAG benchmark proposal is exactly the right way to test this hypothesis.

### 4.2 Datalog is doing too much work, but replacing Soufflé alone will not fix it

The second problem is not that Soufflé is inherently an inappropriate engine.

Datalog is effective at expressing:

- Reachability and dependencies
- Rules over call relationships
- Transitive security policies
- State-transition constraints
- Declarative architectural checks

The problem is using Datalog to compute large, generalized recursive relations and eagerly materialize their results, independently of the review question.

For the older LevelDB profiling baseline, the reported analysis time was approximately distributed as follows:

0 s70 s140 s210 s280 sIngest + SVFWPA loopBatch assemblyPublication

Approximate phase observations from the project's Round 3 Debug profiling; later optimizations changed the total runtime.

Source: performance specifications and scaling investigation.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1

This shows that Datalog evaluation is only part of the measured expense. Input materialization, identity construction, witness canonicalization, serialization, and database publication are substantial contributors.

The `WpaOrchestrator.cpp` implementation confirms sequential SCC/component processing, repeated support collection, and whole-run result retention.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



I recommend changing Datalog's role in three ways:

1. Localize execution. Evaluate relations only within the relevant program slice or affected SCC boundary, passing explicit input/output summaries.
2. Make queries demand-driven. Bind sources, sinks, target functions, and rule predicates before evaluation rather than generating unrestricted closure.
3. Separate derivation from explanation. Determine whether a fact exists first; reconstruct one or a bounded number of useful witness derivations only when needed.

Soufflé supports a magic-set query transformation, which is worth exploring for suitable bound queries. However, its documented implementation has limitations involving functors, negation, and aggregates. VERITAS's current rules employ semantic-key functors and recursive composition, so magic sets cannot simply be enabled and assumed to work.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://www.souffle-lang.com\&sz=32)

souffle-lang.com



The preferred outcome is not necessarily less Datalog code. It is dramatically fewer tuples, smaller working sets, and better alignment between the query and the computation.

### 4.3 Whole-program SVF creates a high fixed cost

The current LLVM/SVF pipeline performs whole-program analysis before the review process knows whether precise pointer analysis is necessary.

The project's measured LevelDB fixture includes approximately 88,563 SVF pointers and 187,055 SVFG nodes, and the profiler records a substantial memory increase during SVF construction.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



This is appropriate for a comprehensive offline static-analysis job, but questionable for every interactive code-review request.

For example:

```
char buffer[32];
memcpy(buffer, input, length);
```

The size of `buffer` is available through local compiler semantics. A full Andersen points-to analysis is unnecessary to establish its declared capacity.

SVF becomes valuable when the destination is a pointer whose target could depend on wrappers, indirect calls, aliases, heap allocations, or interprocedural mutations.

The architecture should support analysis tiers:

| Level | Analysis capability                               | Trigger                       |
| ----- | ------------------------------------------------- | ----------------------------- |
| L0    | Source, AST, symbols, types, direct calls         | Default                       |
| L1    | Local CFG, def-use, simple ranges, object extents | Candidate investigation       |
| L2    | Function contracts and interprocedural summaries  | Cross-function question       |
| L3    | SVF alias/value-flow refinement, solver checks    | Unresolved critical ambiguity |
| L4    | Full WPA, comprehensive audits                    | Explicit deep-analysis mode   |

These are proposed execution levels, distinct from existing SVF alias tiers and EvidenceIR presentation levels.

Importantly, an L3 request is not necessarily cheap: a particular SVF configuration may still require constructing a whole-program base. The new interfaces must report both setup and query costs rather than advertising demand-driven analysis before implementing it.

### 4.4 SummaryDB amplifies data through multiple representations

The seven logical storage layers are architecturally reasonable. The physical representation is less efficient.

The scaling investigation identifies several concrete amplifiers:

| Issue                   | Implementation evidence                                  | Proposed treatment                                       |
| ----------------------- | -------------------------------------------------------- | -------------------------------------------------------- |
| Large persisted closure | Recursive `GlobalFlow`, `ReachableCall`                  | Compact graph plus on-demand queries                     |
| Verbose identifiers     | String-based stable IDs across multiple boundaries       | Interned binary handles; canonical hash at serialization |
| Witness duplication     | Per-derived-fact witness edges and semantic keys         | Lazy proof DAG reconstruction                            |
| Whole-run retention     | `AnalysisFactBatch` and validation retain large payloads | Component-sealed, streaming publication                  |
| Fact serialization      | Hex-encoded protobuf payloads and repeated IDs           | Binary BLOBs, compact column layout                      |
| CAS physical growth     | `PutIfAbsent` implemented using RocksDB `Merge`          | Verified immutable writes, reuse, compaction/GC          |

The `ObjectStoreRocksDb.cpp` implementation indeed calls `DB::Merge` when attempting `PutIfAbsent`. This can accumulate merge operands even for logically unchanged objects. The scale of this contribution remains unmeasured, so it should be profiled before assigning it a large storage-saving target.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1



There is a deeper issue: the identity and provenance model is optimized for proving that a comprehensive fact batch is consistent, rather than for publishing small, independently verifiable semantic results.

I would introduce component manifests and Merkle-style roots for selected fact sets, allowing transactional publication of independently validated components without requiring all facts to remain resident.

This requires a new versioned publication contract. It must not silently weaken today's full-run completeness guarantees.

### 4.5 Incrementality exists conceptually but is not operational end-to-end

This is one of the most actionable correctness/performance issues.

In the current `WpaOrchestrator.cpp` code, a changed component enqueues predecessor work into a `WorklistScheduler`, then drains it into `scheduled_predecessors`.

That collection is reported but not subsequently executed as an incremental worklist.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



The existing component cache can skip some evaluation, but the pipeline still traverses the complete component set and performs substantial analysis and validation work.

Therefore:

Semantic incrementality is currently more advanced in the data model than in the execution architecture.

The redesign should make changes propagate through a real dependency graph:

```
Source / build configuration change
             ↓
Affected translation units
             ↓
Changed Function Summaries
             ↓
Changed SCC input/output contracts
             ↓
Dependent semantic queries / Evidence cases
             ↓
Targeted revalidation
```

An unchanged function should retain its summary and analysis outputs where the validity conditions still hold.

This should also apply to negative evidence. For example, the conclusion that no relevant guard exists depends on which statements, paths, callees, and model assumptions were examined. Adding a guard or changing an indirect callee may invalidate that result.

### 4.6 EvidenceIR is well designed, but its semantic pipeline is incomplete

This weakness is especially important because EvidenceIR is intended to be the bridge to the Agent.

The repository's own correctness audit, issue #149, identifies several material gaps:

- The thin CPG does not emit the containment edges needed for some function-scoped Evidence queries.
- Value-range, destination-capacity, queryable alias-state, and positive dominating-check producers remain absent.
- Overflow candidate resolution is not reliably scoped to one exact sink call site.
- Some complete-empty and open-world result handling is incomplete.
- Summary references and certain provenance-depth controls are not fully carried into the EIR pipeline.
- Program boundaries and target linkage are not sufficiently represented for all compilation databases.&#x20;

  [image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

  GitHub

  +2



For example, the current overflow demonstration can establish that a packet length reaches the size operand of `memcpy`, but it cannot always establish the destination capacity and permitted range necessary to prove an overflow.

The code-review tutorial explicitly describes this as a possible defect rather than a verified one.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



This is not simply missing detector functionality. It demonstrates a broader architectural limitation:

> VERITAS has developed an elaborate mechanism for storing, proving, and explaining relations before the end-to-end semantic meaning and usability of some of those relations have been fully validated.

I would prioritize fixing this semantic integrity problem ahead of adding more analysis domains.

## 5. The central redesign: an LLM-guided, evidence-driven architecture

The proposed architecture would have six major subsystems, with the Agent involved much earlier.

1\. Semantic Code Index

Source · Symbols · Types · Direct Calls · Build Context

2\. LLM Semantic Reasoning Agent

Understand code and contracts · Generate hypotheses · Plan analysis

3\. Capability-Driven Analysis Controller

Select questions, scope, tools, precision, budgets and stopping rules

Local

Clang / LLVM

Cross-function

Summaries / Datalog / SVF

Verification

SMT / Tests / Replay

4\. Semantic SummaryDB

Compact summaries · Graph adjacency · Dependencies · Cached evidence

5\. Claim-Oriented EvidenceIR

Claim · Evidence · Counterevidence · Unknowns · Proof obligations

6\. Agent Review & Verification Loop

Supported findings · Inconclusive cases · Reinvestigation · Semantic memory

The controller can route new evidence and unknowns back to the reasoning Agent for another bounded investigation.

The key architectural change is that the program database becomes a lightweight, incrementally updated semantic substrate rather than the exhaustive product of static analysis.

The existing full WPA engine should remain available as a separate provider and as a differential baseline. The original deterministic analysis is an asset, not a component that needs wholesale replacement.

### 5.1 Technical direction A — Separate structural facts from semantic understanding

I would introduce a two-track analysis model.

Track 1: Deterministic structural facts

This includes symbols, types, function references, call sites, CFG edges, parameter bindings, and source locations.

It must be compiler-grounded and reproducible.

Track 2: Semantic contracts and hypotheses

This includes API behavior, expected state transitions, protocol invariants, resource ownership expectations, error-handling conventions, and relationships described by documentation or tests.

These are areas where an LLM can contribute significantly more than a conventional relational program analysis.

For example, an LLM can infer that a C++ function named `ReleaseConnection()` is expected to invalidate certain session resources, but that expectation must initially be a hypothesis.

VERITAS can then ask deterministic tools whether the code implements that behavior under a specific execution and concurrency model.

This creates a more productive division of responsibility:

| Dimension                        | Deterministic analysis       | LLM                                |
| -------------------------------- | ---------------------------- | ---------------------------------- |
| Syntax, symbols, type resolution | Primary                      | Interpret                          |
| Call graph, data flow            | Primary                      | Select relevant questions          |
| Pointer aliasing                 | Formal approximation         | Identify ambiguity                 |
| Function intent                  | Limited                      | Primary hypothesis generator       |
| API/protocol contract            | Check formalized conditions  | Discover and formalize             |
| State machine completeness       | Analyze reachable states     | Propose missing transitions        |
| Bug finding                      | Validate specific predicates | Discover candidates and prioritize |
| Final verification               | Scope-qualified tools        | Explain conclusions                |

The LLM should not be restricted to EvidenceIR alone. It should receive selected source code, related tests, documentation, and formal evidence.

This is also the direction proposed in the repository's open review-driven design PR.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 5.2 Technical direction B — Evolve Function Summary IR into a Semantic Contract IR

The existing Function Summary IR is excellent for deterministic propagation, but I would add a higher-level semantic contract layer.

A conceptual schema could look like:

```

FunctionContract:
  symbol: "ConnectionManager::Release"
  revision: "<revision-id>"
  build_variant: "<build-id>"

  preconditions:
    - "connection state is CONNECTED"

  effects:
    - "release allocated resources"
    - "invalidate the connection handle"
    - "transition connection state to IDLE"

  invariants:
    - "released connections cannot schedule new work"

  dependencies:
    - "ResourceManager::Free"
    - "Scheduler::RemoveUser"

  unknowns:
    - "asynchronous callback completion order"

  evidence:
    - source_anchor: "<source-reference>"
    - fact_ids: ["<compiler-derived-fact>"]

  authority:
    kind: "LLM_INFERRED"
    verification: "PENDING"

```

This is illustrative rather than an existing VERITAS API.

Crucially, the existing deterministic summaries should not be mixed indiscriminately with LLM-inferred statements. I recommend keeping separate representations for:

- Formal summaries: soundness-qualified compiler and analysis results.
- Semantic contracts: documented, human-specified, or LLM-inferred behavior with explicit authority and uncertainty.

A validated semantic contract can become a reusable verification input, but an unverified summary must never become a source of `MUST` facts merely through repeated reuse.

This higher-level layer gives the Agent meaningful information without requiring it to reconstruct behavior from millions of low-level graph nodes.

### 5.3 Technical direction C — Redesign SummaryDB around compact, queryable semantics

The current SummaryDB's seven-layer logical decomposition is not the root problem. I would simplify what is stored by default.

| Stored permanently           | Derived or reconstructed on demand |
| ---------------------------- | ---------------------------------- |
| Symbol/type index            | Full instruction-level graph views |
| Direct call relationships    | Transitive all-pairs reachability  |
| Function Summary IR          | Deep interprocedural flows         |
| Build and module membership  | Context-sensitive alias results    |
| Component/dependency hashes  | Large provenance witness trees     |
| Source/test/document anchors | Claim-specific evidence slices     |
| Validated reusable contracts | Temporary solver states            |

Suggested storage technologies:

- SQLite or RocksDB: compact metadata and immutable summaries.
- Compressed adjacency indexes: function calls, reverse dependencies, source references.
- CAS: deduplicated summary and Evidence objects.
- Optional semantic/vector index: retrieval and ranking of functions or contracts, never the authority for program facts.

A graph database is not required as the primary storage technology. A graph-like query model can sit over compact adjacency structures and semantic summary APIs.

Meta's Glean is a useful external reference. Its architecture distinguishes source indexing, flexible query-time derivation, and incremental indexing, rather than requiring every derived relationship to be permanently materialized. Its immutable stacked database approach also offers lessons for revision-aware indexing.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://engineering.fb.com\&sz=32)

Engineering at Meta



### 5.4 Technical direction D — Introduce true demand-driven analysis

The core new API should be question-oriented, not database-table-oriented.

For example:

```
AnalyzeRequest {
    claim_kind: "buffer_overflow",
    source: input_length,
    sink: memcpy_callsite,
    requested_evidence: [
        "source_to_sink_flow",
        "destination_capacity",
        "reachable_range",
        "dominating_guards"
    ],
    scope: selected_program,
    budget: bounded_budget
}
```

The controller should choose the cheapest sufficient method, escalating precision only when uncertainty materially affects the decision.

For example:

- For a direct local array: Clang/LLVM local analysis may suffice.
- For a wrapper function: compositional Function Summary IR may suffice.
- For an ambiguous heap pointer: SVF could resolve relevant aliases.
- For a path-condition feasibility question: SMT or symbolic execution may be necessary.
- For a temporal/concurrency issue: a suitable execution trace, schedule exploration, or state model may be required.

This also suggests adding an analysis-cost model.

For capability \\(a\\), estimate:

\\[ \operatorname{Priority}(a) = \frac{\operatorname{ExpectedDecisionValue}(a)} {\operatorname{ExpectedCost}(a)} \\]

This should be treated as a scheduling heuristic, not a soundness criterion. The Agent may prioritize investigations, but it cannot use its own confidence to certify unexamined code safe.

### 5.5 Technical direction E — Turn EvidenceIR into a true hypothesis–verification protocol

EvidenceIR should become the common protocol between the Agent and deterministic reasoning systems.

An Evidence case should support this lifecycle:

```
LLM or deterministic candidate
             ↓
Hypothesis admission
             ↓
Claim + explicit proof obligation
             ↓
Evidence acquisition
             ↓
Contradictions / gaps / assumptions
             ↓
Proof, replay, counterexample or timeout
             ↓
Supported / Verified / Refuted / Inconclusive
             ↓
Versioned review result
```

This is more valuable than treating EvidenceIR only as an output format.

The protocol needs three important guarantees.

First, scoped authority. A `MUST` flow means a relationship is established under the specific static-analysis model; it does not automatically mean the path is executable in the real program. A dynamic test showing a bug proves the occurrence for that input, but not general program safety.

Second, independent verification. An LLM-proposed API contract, state invariant, or postcondition remains conditional until checked against independent evidence. A verifier must not accept the very hypothesis it is supposed to establish as an authoritative assumption.

Third, explicit completeness. Missing results, timeouts, unsupported models, truncated paths, and unresolved external calls must remain visible throughout the Evidence lifecycle.

Recent research reinforces the value of this separation: evaluations of LLM code reviewers have found persuasive but incorrect compliance judgments, while feedback from testing and static analysis can improve reliability.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://link.springer.com\&sz=32)

Springer Nature Link

+1



## 6. Example: where LLM semantics creates value that Datalog alone cannot

Consider a stateful C++ system implementing a connection manager and a scheduling subsystem.

The intended invariant is:

> Once a connection is released, no asynchronous scheduler callback may operate on that connection until it is safely re-established.

An LLM can understand the likely relationship among `ReleaseConnection`, `RemoveUser`, `ScheduleTask`, callback registration, and the connection state machine. It can examine nearby code and tests, identify suspicious event ordering, and propose a missing invariant.

The current Datalog engine can discover structural relationships such as calls and flows. However, without explicitly modeled domain semantics, it does not know that a release event is expected to prohibit a later scheduling event.

The proposed VERITAS approach would look like this:

| Stage                | Action                                                                                      |
| -------------------- | ------------------------------------------------------------------------------------------- |
| Semantic discovery   | Agent infers a lifecycle invariant from code, tests, and specification                      |
| Structural grounding | LLVM/CPG identifies involved functions, state variables, and callback registrations         |
| Hypothesis           | A delayed callback may execute after resource release                                       |
| Selective analysis   | Inspect state writes, callback dispatch, ownership and happens-before relationships         |
| Verification         | Explore an executable event schedule or verify a formal transition model                    |
| Result               | Reproducible counterexample, qualified proof, or explicit unresolved concurrency assumption |

For an asynchronous Actor-style or RRC/MAC implementation, this becomes especially valuable because defects may arise from event ordering and cross-module state contracts rather than simple local memory-flow mistakes.

This is precisely where a neuro-symbolic system can distinguish itself from either conventional static analysis or a pure LLM coding assistant: the LLM contributes domain-level meaning, while the analysis backend checks the relevant execution semantics.

## 7. Refactoring strategy: evolution versus rewrite

I would not restart VERITAS from scratch.

A substantial amount of infrastructure is reusable, and wholesale replacement would discard validated contracts and conformance tests.

Instead, use a strangler-style evolution: add a new lightweight analysis path alongside the existing full WPA path, qualify it, and gradually promote it to the default for interactive review.

### Architectural decisions

| Existing component    | Decision               | Changes                                                                  |
| --------------------- | ---------------------- | ------------------------------------------------------------------------ |
| Clang / LLVM frontend | Keep                   | Add lightweight and persistent per-TU acquisition                        |
| SVF                   | Keep, reposition       | Optional precision provider for interactive work; full mode remains      |
| Function Summary IR   | Extend                 | Add higher-level contracts and explicit summary completeness             |
| Thin CPG              | Keep                   | Prioritize fast symbol/call/containment queries                          |
| Soufflé Datalog       | Retain selectively     | Scoped rule evaluation, avoid unconditional global closure               |
| SummaryDB             | Substantially refactor | Binary IDs, compact graph storage, bounded publication and lifecycle     |
| EvidenceIR            | Extend                 | Hypothesis/verification exchange with clear authority                    |
| Agent orchestration   | Build                  | Planning, semantic understanding, candidate discovery and tool selection |
| Full WPA pipeline     | Preserve               | Offline audits, compatibility and conformance baseline                   |

### Proposed delivery roadmap

### P0

Restore semantic correctness and establish measurement

Fix the documented Evidence integrity problems, including callsite scoping, containment, completeness and missing overflow producers. Run matched Debug/Release benchmarks and measure exact store-byte composition. Preserve the current full WPA pipeline as a baseline.

### P1

Deliver a real lightweight LLM review loop

Implement a structural/local provider, bounded source and test retrieval, Agent candidate discovery, a capability controller, and one real Evidence-backed verification workflow. Use the existing full analyzer only when explicitly requested or when its results are already available.

### P2

Make analysis and storage query-driven

Introduce compact hierarchical IDs, selective relation evaluation, demand-driven path reconstruction, claim-specific Evidence caching, and component-level publication. Reduce mandatory persisted closure and witness data.

### P3

Implement end-to-end semantic incrementality

Connect changed source and build inputs to invalidated summaries, SCC boundaries, analysis models and Evidence dependencies. Validate cross-revision reuse and control physical CAS growth.

### P4

Expand semantic and formal analysis capability

Add reusable Semantic Contract IR, targeted SVF/SMT/replay integration, resource-lifetime verification, asynchronous state machines, API contracts, and selected domain-specific proof obligations.

The most important sequencing decision is that P1 should not wait for completion of every scaling milestone.

A narrow, measurable review product will reveal which sophisticated analyses actually contribute value. It can guide subsequent investment much more effectively than expanding the static-analysis infrastructure in isolation.

## 8. Success metrics and experimental validation

I recommend changing the project's primary success criterion from:

> How much whole-program semantic information can VERITAS derive and persist?

to:

> How accurately and efficiently can VERITAS answer consequential program-behavior questions, with evidence that users can inspect and trust?

That requires both systems measurements and analysis-quality measurements.

### 8.1 Proposed engineering targets

These are proposed acceptance targets, not results achieved by the project.

| Dimension                         | Suggested initial target                                                               |
| --------------------------------- | -------------------------------------------------------------------------------------- |
| Lightweight structural index size | At least 80–90% smaller than the existing full-analysis store on matched fixtures      |
| Warm structural queries           | P95 under 2 seconds on the baseline fixture                                            |
| Interactive first review          | P95 under 30 seconds for a pre-indexed, bounded change, subject to model latency       |
| No-change incremental run         | No substantive recomputation or unbounded CAS growth                                   |
| Single-function edit              | Cost proportional to the measured invalidation cone plus unavoidable frontend costs    |
| Full-analysis memory              | Demonstrable reduction versus current Debug baseline, with Release measured separately |
| Evidence completeness             | Zero false promotion of unsupported/incomplete results in qualification tests          |

The initial percentages and latencies are product engineering targets to validate, not predictions. Cold index construction, model inference latency, and optional deep-analysis cost must be reported separately.

### 8.2 Evaluate whether the LLM and analysis engine actually improve each other

Run an ablation study with the same benchmark and time/resource budget.

| Variant | Description                                  | What it measures                         |
| ------- | -------------------------------------------- | ---------------------------------------- |
| A       | LLM + bounded source context                 | Semantic reasoning baseline              |
| B       | LLM + lightweight symbol/function summaries  | Value of the semantic index              |
| C       | B + local compiler-derived evidence          | Value of cheap deterministic analysis    |
| D       | C + selective SVF/Datalog/SMT                | Marginal value of deeper formal analysis |
| E       | Existing comprehensive WPA/Evidence pipeline | Full-analysis quality/cost reference     |

Track precision, recall, false-positive rate, missed critical defects, evidence correctness, human review effort, latency, memory, storage, and LLM token consumption.

Use both fixed-candidate tests and end-to-end discovery tests. Fixed-candidate tests isolate verification capability; discovery tests reveal whether the LLM overlooks important defects before analysis begins.

The benchmark corpus should span:

- Local memory/bounds defects
- Multi-translation-unit flow and aliasing
- API contract violations
- Missing state transitions
- Asynchronous ordering and resource lifetime
- Negative controls with correct code
- Edits that modify only an apparently unrelated caller or configuration
- Large repositories with realistic build variants

For scalability, retain the project's proposed chain/star/layered-DAG generators alongside 10 KLOC, 100 KLOC, and 1 MLOC real projects. Do not extrapolate the LevelDB footprint directly into a reliable industrial sizing estimate.

Go/no-go criterion: Deeper analysis should earn its place by improving adjudicated review quality or verification coverage at an acceptable incremental cost. If it does not, retain it in offline mode rather than making it a mandatory interactive dependency.

## 9. Priority engineering issues I would address first

Based on the inspected code and issue tracker, these are the highest-leverage near-term work items.

| Priority | Concrete change                                                            | Existing tracking                                                                                                                                         |
| -------- | -------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Critical | Fix Evidence completeness, containment, authority and provenance defects   | [#149](https://github.com/skg7on/VERITAS/issues/149?utm_source=chatgpt.com)                                                                               |
| Critical | Resolve exact sink call sites and missing range/capacity/alias/check facts | [#132](https://github.com/skg7on/VERITAS/issues/132?utm_source=chatgpt.com), [#124](https://github.com/skg7on/VERITAS/issues/124?utm_source=chatgpt.com)  |
| Critical | Implement lightweight source/local analysis + real Review Agent controller | [#151](https://github.com/skg7on/VERITAS/issues/151?utm_source=chatgpt.com), [PR #152](https://github.com/skg7on/VERITAS/pull/152?utm_source=chatgpt.com) |
| High     | Make WPA invalidation executable, not just reported                        | [#143](https://github.com/skg7on/VERITAS/issues/143?utm_source=chatgpt.com)                                                                               |
| High     | Eliminate unchanged-run physical CAS amplification                         | [#144](https://github.com/skg7on/VERITAS/issues/144?utm_source=chatgpt.com)                                                                               |
| High     | Establish realistic storage/closure scaling measurements                   | [#140](https://github.com/skg7on/VERITAS/issues/140?utm_source=chatgpt.com)                                                                               |
| High     | Fix host-dependent function identity before portable reuse                 | [#125](https://github.com/skg7on/VERITAS/issues/125?utm_source=chatgpt.com)                                                                               |
| Medium   | Refactor full-analysis persistence and recursive closure                   | M15–M19 scaling program, conditioned on measurement                                                                                                       |

The first meaningful delivery should be a production-capable review of a small real C/C++ change, with one analytical defect and one semantic contract issue, both backed by scoped, inspectable evidence.

It should support useful findings even when the full WPA database does not exist.

That would validate the future architecture much more convincingly than another large set of persisted Datalog relations.

## 10. Final architectural recommendation

I see three possible strategic directions.

| Strategy                                                                             | Assessment                                                                                           |
| ------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------- |
| Continue expanding the full CPG/WPA/Soufflé database and optimize its implementation | Suitable for an offline analysis platform, but expensive and unlikely to fully exploit LLM semantics |
| Replace most static analysis with LLM source-code reasoning                          | Fast to prototype, but weak on program facts, completeness and verification                          |
| Build an LLM-guided reasoning engine backed by selective formal analysis             | Best balance of semantic understanding, scaling potential and evidence quality                       |

The third direction gives VERITAS a distinctive technical identity.

Its competitive value should not be the size of the Code Property Graph, the number of generated relations, or even the sophistication of its Datalog rules.

It should be the ability to combine three capabilities that are individually incomplete:

LLM

Understands intent, contracts and context

Static Analysis

Establishes precise structural relationships

Verification

Checks scoped claims and assumptions

## Semantic Understanding × Program Evidence × Verifiable Reasoning

I would express VERITAS's next-generation architectural thesis as:

> VERITAS is an evidence-driven program reasoning platform in which LLMs discover semantic questions, compositional program summaries provide reusable context, and selective formal analysis establishes or refutes claims with explicit scope, uncertainty, and provenance.

The most important design inversion is:

From “analyze everything, store everything, then ask the Agent” to “understand the question, retrieve the relevant program semantics, analyze what is necessary, and verify the conclusion.”

This preserves the project's strongest foundations while directly addressing database explosion, Datalog latency, and underutilization of LLM semantic reasoning.

Review scope: public `main` source files, architecture documents, performance specifications, GitHub issues and pull requests, plus the unmerged review-driven design branch. I did not obtain a local build of the repository or independently reproduce its benchmarks. Reported performance and test results are therefore attributed to the project's published measurements, while code-level observations are based on inspected source.