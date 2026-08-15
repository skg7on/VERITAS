# VERITAS Whole Program Analysis Summary DB Architecture Design

## 1. Purpose

VERITAS is an evidence-centric whole-program analysis platform that combines deterministic static analysis with LLM-assisted semantic reasoning.

The **SummaryDB** is the foundational infrastructure of VERITAS. Its purpose is not merely to cache function metadata, but to create a persistent, incrementally maintainable semantic representation of a very large software system.

The core architectural idea is:

```text
Source Code
    ↓
High-precision Local Static Analysis
    ↓
Function / Global / Type / Resource Summaries
    ↓
SummaryDB
    ↓
Incremental Whole-Program Analysis
    ↓
Evidence Builder
    ↓
Evidence IR
    ↓
LLM Review Agent
```

The SummaryDB should allow VERITAS to answer questions such as:

* What are the externally visible semantic effects of function `F`?
* Which functions may modify object `X`?
* Which call paths propagate value `V` into sink `S`?
* What changed semantically between two commits?
* Which summaries must be recomputed after a local source change?
* What downstream code is affected by a changed function contract?
* Which static-analysis facts are certain, approximate, inferred, or unknown?
* What minimum program slice should be materialized into an Evidence IR case?

The architectural principle is similar to LLVM ThinLTO: individual compilation units generate compact summaries, and whole-program analysis operates primarily over a combined summary index rather than repeatedly loading complete IR for every module.

---

# 2. SummaryDB is not one database

The name “SummaryDB” should refer to a **logical subsystem**, not a single physical database technology.

I recommend the following architecture:

```text
                     VERITAS SummaryDB
                           │
        ┌──────────────────┼──────────────────┐
        │                  │                  │
        ▼                  ▼                  ▼
   Object Store        Fact Store        Graph Index
        │                  │                  │
 Immutable              Normalized           CPG
 summaries              relations        semantic graph
        │                  │                  │
        └──────────┬───────┴──────────┬──────┘
                   │                  │
                   ▼                  ▼
              WPA Engine         Query Service
             Datalog/fixpoint    Agent/Evidence
```

The recommended physical separation is:

| Layer                | Responsibility                              |
| -------------------- | ------------------------------------------- |
| Summary Object Store | canonical immutable summaries               |
| Metadata Store       | versions, builds, configurations, ownership |
| Fact Store           | normalized semantic relations               |
| Graph Index          | graph navigation and CPG queries            |
| Dependency Index     | incremental invalidation                    |
| WPA Engine           | recursive interprocedural analysis          |
| Evidence Cache       | materialized slices for review              |
| History Store        | semantic diffs across commits               |

This separation avoids forcing all program-analysis workloads into a property graph.

---

# 3. Overall project architecture

```text
                         Git / Build System
                                │
                     compile_commands.json
                                │
                                ▼
                    ┌─────────────────────┐
                    │ Build Intelligence  │
                    │                     │
                    │ target/config/macros│
                    │ include graph       │
                    │ TU dependency       │
                    └──────────┬──────────┘
                               │
              ┌────────────────┼──────────────────┐
              ▼                ▼                  ▼
         Clang AST         LLVM IR          Debug/ABI info
              │                │                  │
              └────────────────┼──────────────────┘
                               ▼
                  Local Static Analysis Engine
                               │
          ┌────────────────────┼────────────────────┐
          ▼                    ▼                    ▼
      AST/CFG              SSA/VFG              CPG Facts
          │                    │                    │
          ├─ calls             ├─ defs/uses         ├─ nodes
          ├─ types             ├─ alias             ├─ edges
          ├─ templates         ├─ ranges            ├─ source
          ├─ macros            ├─ memory            ├─ controls
          └─ source map        └─ taint             └─ relations
                               │
                               ▼
                     Function Summary IR
                               │
                               ▼
                         SummaryDB
                               │
             ┌─────────────────┼────────────────┐
             ▼                 ▼                ▼
         Local facts      Dependency DAG    Graph projection
             │                 │                │
             ▼                 ▼                ▼
                   Incremental WPA Engine
                               │
                        SCC/fixpoint
                               │
                               ▼
                    Global Semantic Facts
                               │
                               ▼
                       Evidence Builder
                               │
                               ▼
                         Evidence IR
                               │
                               ▼
                       Review Agent
```

---

# 4. Static analysis frontend: start with Clang + LLVM

For C/C++, I would strongly recommend using **Clang and LLVM as the initial analysis frontend**, rather than implementing a parser, type system, CFG, or SSA infrastructure yourself.

Clang gives VERITAS access to:

```text
C/C++ semantic AST
templates
macros
canonical declarations
type system
source locations
declaration USRs
CFG
compile configuration
```

LLVM IR adds:

```text
SSA
explicit memory operations
canonical control flow
def-use chains
target-aware semantics
optimization-normalized representation
```

These two levels are complementary.

## Recommended split

```text
Clang AST
   │
   ├── semantic identity
   ├── language constructs
   ├── source mapping
   ├── templates
   ├── inheritance
   ├── virtual dispatch
   └── macro provenance

LLVM IR
   │
   ├── SSA
   ├── CFG
   ├── value flow
   ├── memory operations
   ├── alias analysis
   ├── range analysis
   └── low-level effects
```

Do not choose only one.

LLVM IR alone loses useful source-level semantics.

AST alone makes advanced value-flow analysis substantially harder.

---

# 5. Build a VERITAS CPG, but keep it thin

Joern's Code Property Graph architecture is useful inspiration: a CPG provides a unified graph representation over syntactic and semantic program structures and enables graph-based mining of large codebases.

VERITAS should adopt the *concept* of a CPG but avoid copying an unnecessarily huge universal graph.

I recommend:

```text
VERITAS CPG

Nodes:
    TranslationUnit
    Namespace
    Type
    Function
    Parameter
    Local
    Global
    BasicBlock
    Instruction
    CallSite
    MemoryObject
    Field
    Lock
    State
    Thread
    Message

Edges:
    CONTAINS
    DECLARES
    CALLS
    MAY_CALL
    DEF
    USE
    FLOWS_TO
    CONTROLS
    DOMINATES
    POST_DOMINATES
    READS
    WRITES
    ALIASES
    ACQUIRES
    RELEASES
    TRANSITIONS
```

The persistent graph should usually be **function- and object-centric**, not instruction-centric for the entire repository.

Detailed instruction-level graphs can be generated or cached on demand.

This distinction will matter enormously at 10M–100M LOC scale.

---

# 6. Static analysis engines required for V1

The initial analyzer should not attempt every possible sophisticated analysis.

Start with six core engines.

## 6.1 Call Graph Analysis

Support:

```text
direct calls
function pointers
virtual dispatch
callbacks
templates
external calls
unknown targets
```

Represent certainty explicitly:

```text
MUST_CALL
MAY_CALL
UNKNOWN_CALL
```

Example:

```text
CallEdge {
    caller
    callsite
    target
    confidence
    dispatch_kind
}
```

Call graph quality is foundational because almost every WPA algorithm depends on it.

---

## 6.2 CFG and dominator analysis

Store summary-level facts such as:

```text
dominates(validate, memcpy)
postdominates(cleanup, allocation)
reachable(A, B)
```

Avoid permanently storing every CFG node centrally unless needed.

The source frontend can regenerate detailed CFGs.

---

## 6.3 Def-use / value-flow analysis

CodeQL distinguishes local data flow, global data flow, and taint tracking for C/C++, which is a useful conceptual decomposition for VERITAS.

VERITAS should similarly produce:

```text
LocalDefUse
ParameterToReturn
ParameterToGlobal
GlobalToReturn
FieldToField
CallArgToParameter
ReturnToCallResult
```

This becomes the basis of the VFG.

---

# 7. VFG should be more important than AST for Evidence generation

The AST is excellent for understanding syntax.

The VFG is much more useful for explaining defects.

Example:

```text
packet->len
     │
     ▼
parseHeader::len
     │
     ▼
decodeIE::size
     │
     ▼
copyIE::length
     │
     ▼
memcpy.size
```

Evidence generation wants this graph directly.

Therefore one important intermediate representation is:

```text
ValueNode

ValueFlowEdge {
    src
    dst

    kind:
       assignment
       parameter
       return
       load
       store
       phi
       alias
       summary

    condition
}
```

---

# 8. Alias and points-to analysis

For V1, I would avoid trying to implement the ultimate flow/context-sensitive pointer analysis immediately.

Use layered precision.

```text
Level 0
LLVM BasicAA / type-based alias

Level 1
Andersen-style inclusion analysis

Level 2
Context-sensitive refinement

Level 3
Demand-driven path refinement
```

The SummaryDB should store:

```text
MustAlias
MayAlias
NoAlias
UnknownAlias
```

Never collapse uncertainty to a boolean.

For Evidence IR, unknown aliasing is valuable information itself.

---

# 9. Memory-effect analysis

Every function should receive a compact effect summary:

```yaml
memory:
  reads:
    - ctx.state
    - packet.header

  writes:
    - ctx.counter
    - ctx.state

  allocates:
    - return

  frees:
    - arg1

  escapes:
    - arg0
```

Use abstract memory locations rather than raw LLVM pointers.

For example:

```text
Global:G

Object:ctx
ObjectField:ctx.state

ArgumentObject:arg0
ArgumentField:arg0.header

HeapSite:foo.cpp:128
```

Memory abstraction quality is one of the most important decisions in the whole project.

---

# 10. Range and constraint analysis

Build on LLVM's existing value reasoning initially.

Produce summaries such as:

```text
0 <= packet.len <= 65535

SUCCESS =>
    ctx.state == CONNECTED

arg0 != NULL

ret == SUCCESS =>
    bytes_written <= capacity
```

Eventually the representation should support symbolic expressions:

```text
RangeExpression
BooleanConstraint
LinearConstraint
Predicate
```

These facts later feed Evidence IR and SMT.

---

# 11. Function Summary IR

The initial summary schema could be:

```text
FunctionSummary {

    identity

    build_variant

    source_hash

    body_hash

    signature

    calls[]

    memory_effects[]

    value_transfers[]

    range_facts[]

    alias_facts[]

    taint_transfers[]

    ownership_effects[]

    locks[]

    state_transitions[]

    assumptions[]

    unknowns[]

    dependencies[]

    provenance[]
}
```

Example:

```yaml
function:
  symbol: Decoder::decodeIE
  variant: ARM64_RELEASE_A

inputs:
  - packet
  - context

reads:
  - packet.len
  - context.state

writes:
  - context.state

calls:
  - validateIE
  - copyPayload

value_flow:
  - packet.len -> copyPayload.arg2

range:
  packet.len:
    min: 0
    max: 65535

state:
  - VALIDATING -> COMPLETE

unknowns:
  - semantics: vendorValidate()

dependencies:
  - function: validateIE
  - type: Packet
  - global: gConfig
```

---

# 12. Summary identity

Never use file+line as function identity.

Use two levels.

```text
FunctionSymbolID
```

represents logical identity:

```text
hash(
    mangled symbol
    canonical signature
    linkage context
)
```

Then:

```text
FunctionVariantID
```

represents build semantics:

```text
hash(
    FunctionSymbolID
    target
    ABI
    macro configuration
    compile options
    relevant headers
)
```

Then:

```text
FunctionSummaryID
```

represents actual semantic content:

```text
hash(
    FunctionVariantID
    semantic body hash
    analyzer version
    analysis configuration
)
```

This enables deduplication and cache reuse.

---

# 13. Content-addressable storage

Summary objects should be immutable.

```text
SummaryID
    ↓
serialized FunctionSummary
```

Then mappings provide:

```text
Build
    ↓
FunctionVariant
    ↓
SummaryID
```

This architecture naturally supports:

```text
multiple branches
historical revisions
shared functions
parallel builds
distributed workers
```

and avoids mutating existing semantic facts.

---

# 14. Incremental analysis is the centerpiece

The most important SummaryDB capability is not persistence.

It is:

> **semantic incremental recomputation.**

Suppose:

```text
A → B → C → D
```

and `D` changes.

Naive approach:

```text
recompute D
recompute C
recompute B
recompute A
```

VERITAS should instead do:

```text
recompute D

old_summary(D)
       vs
new_summary(D)

       │
       ├── identical
       │       ↓
       │      STOP
       │
       └── changed
               ↓
           invalidate C
```

Then:

```text
summary(C) changed?
       │
       ├─ no → stop
       │
       └─ yes → invalidate B
```

This is **semantic invalidation**, not merely dependency invalidation.

---

# 15. Dependency graph design

Each summary must explicitly record dependencies.

```text
FunctionSummary F
      │
      ├── calls G
      ├── reads Global X
      ├── uses Type T
      ├── relies on AliasFact A
      ├── uses Configuration C
      └── imports Summary S
```

Represent:

```text
DependencyEdge {
    consumer
    producer

    kind:
       CALL
       TYPE_LAYOUT
       GLOBAL_VALUE
       ALIAS
       SUMMARY
       BUILD_CONFIG
       MACRO
       CONTRACT
}
```

This creates a reverse invalidation graph.

---

# 16. Not all source changes should trigger propagation

Semantic hashes should be computed at several levels.

```text
SourceHash
ASTHash
IRHash
SummaryHash
```

Suppose:

```cpp
int x = a + b;
```

becomes:

```cpp
int x = b + a;
```

The source hash changes.

But:

```text
FunctionSummary
```

may remain identical.

WPA propagation should stop there.

Similarly:

```text
comment change
```

should normally never reach the analyzer.

---

# 17. Summary components should have independent hashes

Even better:

```text
FunctionSummary
   │
   ├── CallHash
   ├── EffectHash
   ├── ValueFlowHash
   ├── RangeHash
   ├── OwnershipHash
   ├── StateHash
   └── LockHash
```

Why?

Suppose only:

```text
range summary
```

changes.

Then a call-graph analysis consumer does not need invalidation.

This enables **analysis-specific incremental propagation**.

Example:

```text
Change:
RangeHash

invalidate:
buffer bounds analysis
taint sanitization analysis

do not invalidate:
call graph
locking analysis
type graph
```

This is a major scalability improvement.

---

# 18. SCC-aware WPA

Recursive call graphs require fixpoint analysis.

Example:

```text
A → B
↑   ↓
D ← C
```

Treat:

```text
{A,B,C,D}
```

as a strongly connected component.

Then compute:

```text
Summary(SCC)

while changed:
    evaluate members
    join summaries
```

After convergence:

```text
SCCSummaryHash
```

can itself participate in incremental propagation.

If an internal change does not change the SCC's externally visible summary:

```text
STOP propagation
```

---

# 19. WPA Fact Engine

I recommend adding Datalog relatively early.

Soufflé is particularly appropriate because Datalog supports recursive relations naturally and is designed for program-analysis workloads.

Example:

```text
MayWrite(f,x) :-
    DirectWrite(f,x).

MayWrite(f,x) :-
    Call(f,g),
    MayWrite(g,x).
```

Or:

```text
Reachable(f,g) :-
    Call(f,g).

Reachable(f,h) :-
    Call(f,g),
    Reachable(g,h).
```

This is much cleaner than implementing every transitive analysis manually.

Soufflé provenance is also particularly relevant to VERITAS because derived tuples can be explained through proof trees.

That capability can feed directly into Evidence IR.

---

# 20. Provenance must be built in from day one

Every important fact should answer:

```text
Why is this true?
```

Example:

```text
MayWrite(A, GlobalX)
```

might have provenance:

```text
A
 └─ calls B
      └─ calls C
           └─ direct write GlobalX
```

Represent:

```text
Fact {
    FactID

    predicate

    certainty

    producer

    inputs[]

    source_location

    analyzer_version
}
```

This turns SummaryDB from an opaque analysis cache into a **proof-producing infrastructure**.

---

# 21. Recommended storage architecture

A practical implementation:

```text
                   Summary Service
                         │
       ┌─────────────────┼──────────────────┐
       ▼                 ▼                  ▼
    RocksDB          PostgreSQL          Parquet
       │                 │                  │
summary objects       metadata        offline analytics
local indexes          builds
                      revisions
                         │
                         ▼
                   Fact Engine
                     Soufflé
                         │
                         ▼
                   Graph Index
```

For V1, you can simplify further:

```text
RocksDB
+
Soufflé files
+
memory graph
```

Do not introduce Neo4j immediately unless graph visualization/querying is already a strong requirement.

---

# 22. Incremental update pipeline

The end-to-end update flow should be:

```text
Git Change
    │
    ▼
Changed File Detection
    │
    ▼
Build Dependency Resolver
    │
    ▼
Affected Translation Units
    │
    ▼
AST semantic diff
    │
    ▼
Changed Functions
    │
    ▼
Local Analysis
    │
    ▼
New FunctionSummary
    │
    ▼
Component-level summary diff
    │
    ▼
Semantic Dependency Index
    │
    ▼
Affected consumers
    │
    ▼
SCC propagation
    │
    ▼
Fixpoint
    │
    ▼
Updated WPA facts
```

---

# 23. The update scheduler

Implement the update engine as a worklist.

Conceptually:

```cpp
while (!worklist.empty()) {
    Node n = worklist.pop();

    Summary old = db.get(n);
    Summary fresh = recompute(n);

    SummaryDelta delta = diff(old, fresh);

    if (delta.empty())
        continue;

    db.publish(fresh);

    for (Consumer c : dependencyIndex.users(n, delta))
        worklist.push(c);
}
```

The important part is:

```text
dependencyIndex.users(n, delta)
```

not simply:

```text
all callers(n)
```

because each consumer cares about different summary dimensions.

---

# 24. Distributed scaling model

Once the local version works, SummaryDB naturally supports distributed analysis.

```text
                        Coordinator
                            │
             ┌──────────────┼───────────────┐
             ▼              ▼               ▼
         Worker 1        Worker 2         Worker N
          TU-A            TU-B             TU-Z
             │              │               │
             └──────────────┼───────────────┘
                            ▼
                     Summary Object Store
                            │
                            ▼
                   Global WPA Coordinator
```

Because summaries are immutable and content-addressed, workers can safely generate them in parallel.

---

# 25. CPG generation pipeline

The CPG builder can evolve through three stages.

## Stage 1

```text
AST
+
CFG
+
Call Graph
```

Enough to support:

```text
navigation
call relationships
control flow
source locations
```

## Stage 2

Add:

```text
DFG
VFG
Memory objects
Alias edges
```

Now Evidence IR becomes powerful.

## Stage 3

Add domain semantic graphs:

```text
Lock graph
Thread/task graph
State graph
Message graph
Resource graph
```

This is where VERITAS begins differentiating itself strongly from generic CodeQL/Joern-like systems.

---

# 26. SummaryDB APIs

Avoid exposing SQL/graph internals to users.

Define semantic APIs:

```text
getFunctionSummary(F)

getSummaryAtRevision(F,R)

getSummaryDelta(F,R1,R2)

getCallers(F)

getCallees(F)

getTransitiveCallers(F)

getValueFlow(src,dst)

getMayWrites(F)

getReads(F)

getAliases(V)

getRanges(V)

getStateTransitions(F)

getDependencySet(F)

getImpactSet(Change)

explainFact(FactID)

getEvidenceSlice(Claim)
```

These APIs will later become Review Agent tools.

---

# 27. Relationship with Evidence IR

SummaryDB should not generate full Review Agent prompts.

Its role is:

```text
SummaryDB
    │
    │ semantic query
    ▼
Evidence Builder
    │
    │ slicing + provenance
    ▼
Evidence IR
```

For example:

```text
Claim:
possible overflow memcpy()
```

Evidence Builder asks:

```text
getValueFlow(packet.len, memcpy.size)

getRange(packet.len)

getObjectCapacity(dst)

findDominatingChecks(memcpy)

getCallPath(entry, memcpy)

getRelevantSummaries(path)
```

Then constructs a compact Evidence IR case.

---

# 28. Recommended first project milestone

Do not initially build:

```text
LLM Agent
SMT
symbolic executor
distributed WPA
full security analyzer
```

Build the semantic substrate first.

### Milestone 1 — C/C++ SummaryDB prototype

Target:

```text
~100K–1M LOC repository
```

Capabilities:

```text
Clang compilation database ingestion

stable FunctionID

AST function extraction

CFG generation

call graph

basic memory read/write

LLVM-based value flow

function summary serialization

RocksDB summary storage

summary hash/delta

reverse dependency index

incremental recomputation
```

---

# 29. Milestone 2 — WPA

Add:

```text
SCC detection

summary propagation

transitive effects

basic alias

global value flow

Soufflé fact export

recursive queries

fact provenance
```

At this point you already have a valuable standalone static-analysis infrastructure.

---

# 30. Milestone 3 — CPG

Add:

```text
persistent code graph

VFG

object graph

control dependence

summary edges
```

Queries such as:

```text
Find all flows from external input
to memcpy length
without a dominating range check.
```

should become possible.

---

# 31. Milestone 4 — Evidence IR

Build:

```text
Finding
   ↓
Program slice
   ↓
Evidence Graph
   ↓
Claim
Constraints
Paths
Unknowns
Provenance
```

This connects static analysis to the future Agent.

---

# 32. Milestone 5 — LLM semantic reasoning

Only now introduce:

```text
Review Agent
```

Its initial capabilities should be limited to:

```text
interpret evidence

rank findings

identify missing semantics

infer API contracts

request additional analysis
```

Do **not** allow it to suppress deterministic findings without verification.

Recent neuro-symbolic systems provide support for this architecture. For example, MemHint uses an LLM to infer project-specific allocation/deallocation semantics, validates those summaries with CFG/Z3 reasoning, then injects validated knowledge into traditional analyzers.

---

# 33. Suggested repository structure

A clean initial VERITAS repository might look like:

```text
veritas/

  frontend/
    clang/
    llvm/

  ir/
    summary/
    evidence/
    graph/

  analysis/
    callgraph/
    cfg/
    dataflow/
    alias/
    range/
    memory/
    ownership/

  cpg/
    builder/
    schema/
    query/

  summarydb/
    object_store/
    metadata/
    dependency/
    versioning/

  wpa/
    worklist/
    scc/
    datalog/
    fixpoint/

  evidence/
    builder/
    slicing/
    provenance/

  agent/
    tools/
    reasoning/

  runtime/
    scheduler/
    distributed/

  tests/

  tools/
    veritas-build
    veritas-query
    veritas-diff
    veritas-explain
```

---

# 34. Initial command-line workflow

I would make the developer experience simple from the start.

```bash
veritas-build configure \
    --compile-db build/compile_commands.json
```

Then:

```bash
veritas-build index .
```

Output:

```text
Translation Units:       8,421
Functions:             692,812
Call Edges:          4,281,923
Value-flow Edges:   19,281,372
Summaries:             681,221
Unknown Calls:           8,231
```

Then:

```bash
veritas-query summary Decoder::decodeIE
```

or:

```bash
veritas-query callers Decoder::decodeIE
```

or:

```bash
veritas-query writes gSchedulerState
```

and:

```bash
veritas-diff HEAD~1 HEAD
```

could produce:

```text
Source changed functions:          214

Summary changed functions:          37

Call behavior changed:               4
Memory effects changed:             12
Range contracts changed:             9
State transitions changed:           3

WPA affected functions:            186
```

That last result would be an excellent demonstration of the architecture's value.

---

# 35. First high-value demo

I would make the first impressive VERITAS demonstration extremely focused.

Start from:

```cpp
void decode(Packet *p, Buffer *b)
{
    ...
    memcpy(b->data, p->payload, p->len);
}
```

Show:

```text
AST
 ↓
CFG
 ↓
VFG
 ↓
Function Summary
 ↓
SummaryDB
```

Then modify some upstream function:

```text
validatePacket()
```

and demonstrate:

```text
1 function modified

           ↓

1 local summary recomputed

           ↓

range contract changes

           ↓

only 7 dependent summaries invalidated

           ↓

WPA finds new unsafe flow

           ↓

Evidence IR generated

packet.len
    ↓
decode
    ↓
copy
    ↓
memcpy

No dominating check

           ↓

Review Agent
```

That single demo demonstrates almost the entire VERITAS thesis.

---

# 36. Architectural principles to lock down early

I would formalize these as VERITAS design rules.

### P1 — Semantic summaries are immutable

Never mutate a summary in place.

---

### P2 — Everything important is versioned

Including:

```text
source
build
analysis
schema
summary
fact
```

---

### P3 — Every fact has provenance

No unexplained derived fact.

---

### P4 — Uncertainty is explicit

Use:

```text
MUST
MAY
UNKNOWN
INFERRED
```

rather than pretending all analysis results have equal certainty.

---

### P5 — Incrementality operates on semantic deltas

Not just file changes.

---

### P6 — WPA consumes summaries by default

Load detailed AST/IR only when refinement requires it.

This follows the same broad scalability insight demonstrated by ThinLTO's compact module summaries and combined summary index.

---

### P7 — CPG is a query projection, not the only source of truth

This keeps storage and computation flexible.

---

### P8 — LLM output is initially a hypothesis

Only deterministic analysis can promote it into verified knowledge.

---

# 37. Minimum viable technology stack

My recommended starting stack is deliberately conservative:

```text
Language:
    C++20

Frontend:
    Clang LibTooling
    LLVM

Serialization:
    Protobuf

Summary store:
    RocksDB

Metadata:
    SQLite initially
    PostgreSQL later

WPA:
    C++ worklist engine initially
    Soufflé as recursive analysis grows

CPG:
    custom typed adjacency/index layer initially

Constraint solving:
    Z3 later

Agent:
    Python/C++ service boundary later
```

I would specifically avoid introducing a large distributed database or graph platform during the first implementation.

The hardest problems in VERITAS are going to be:

```text
semantic identity

summary design

memory abstraction

cross-function propagation

precision/scalability tradeoff

incremental invalidation

evidence provenance
```

not database throughput.

---

# 38. Target architecture

The eventual VERITAS architecture can be summarized as:

```text
              ┌───────────────────────────────┐
              │          Source World         │
              │ C/C++ / Build / Specs / Tests │
              └───────────────┬───────────────┘
                              ▼
                   Static Analysis Frontend
                   Clang + LLVM + Domain
                              │
                              ▼
                  ┌─────────────────────┐
                  │   FunctionSummary   │
                  │         IR          │
                  └──────────┬──────────┘
                             │
                             ▼
        ┌──────────────── SummaryDB ────────────────┐
        │                                           │
        │ Immutable Summary Store                   │
        │ Semantic Fact Store                       │
        │ Dependency Index                          │
        │ Version History                           │
        │ CPG Projection                            │
        │                                           │
        └────────────────────┬──────────────────────┘
                             │
                     Incremental WPA
                        SCC / Datalog
                             │
                             ▼
                       Global Facts
                             │
                             ▼
                     Evidence Builder
                             │
                             ▼
                       Evidence IR
                             │
                        ┌────┴────┐
                        ▼         ▼
                     Agent     Proof Engine
                        │         │
                        └────┬────┘
                             ▼
                      Review Result
```

## Final recommendation

For VERITAS, I would make **SummaryDB + incremental WPA the first major engineering milestone**, before building a sophisticated LLM reviewer.

The critical sequence should be:

```text
Clang/LLVM
    ↓
accurate program facts
    ↓
stable Function Summary IR
    ↓
immutable SummaryDB
    ↓
dependency-aware incremental update
    ↓
SCC/fixpoint WPA
    ↓
CPG/VFG query layer
    ↓
Evidence IR
    ↓
LLM Agent
```

If the first six layers are strong, the Review Agent becomes dramatically simpler: instead of trying to “understand a million-line repository,” it reasons over a compact semantic world already built by VERITAS.

That is the technical moat I would aim to build first.

