> **Status:** Brainstorm input retained for design history. The approved
> architecture has been consolidated into
> [`m12-joern-cpg-summarydb-importer-design-spec.md`](../specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md).
> The canonical specification preserves Joern as an optional provider, chooses
> direct GraphSON/GraphML ingestion, integrates normalized provider projections
> into SummaryDB, and supersedes proposals below that would let Joern-derived
> data directly drive native summaries or WPA.

Joern is a very good candidate for the **first production-grade program-structure provider for VERITAS**. I would not, however, make VERITAS “a Joern-based system.” The right abstraction is:

> **Joern = one Evidence Provider**
> **VERITAS = canonical program/evidence model + whole-program reasoning + multi-analyzer evidence fusion**

I want add a “GPG importer” to VERITAS. It means a **Joern CPG importer**. Internally, I would introduce a VERITAS representation tentatively called **VPG — VERITAS Program Graph** rather than reuse the term CPG.

Joern's CPG is particularly attractive because its schema already defines a directed, edge-labeled attributed multigraph with layers for AST, CFG, call graph, dominators, PDG, types, tags, and other relations. Important relations such as `CFG`, `CALL`, `REF`, `REACHING_DEF`, `CDG`, `ARGUMENT`, and `RECEIVER` are explicitly modeled. ([Code Property Graph][1])

---

# 1. Where Joern should sit in VERITAS

I would evolve the VERITAS architecture toward this:

```text
                         VERITAS
┌──────────────────────────────────────────────────────────────────────┐
│                                                                      │
│  Evidence Providers                                                  │
│                                                                      │
│  ┌────────────┐ ┌────────────┐ ┌────────────┐ ┌─────────────────┐   │
│  │   Joern    │ │ Clang/LLVM │ │ SVF/Soufflé│ │ Dynamic / LLM   │   │
│  │ CPG Engine │ │  Analyzer  │ │ WPA Engine │ │ Evidence        │   │
│  └─────┬──────┘ └─────┬──────┘ └─────┬──────┘ └────────┬────────┘   │
│        │              │              │                  │            │
│        ▼              ▼              ▼                  ▼            │
│  ┌──────────────────────────────────────────────────────────────┐    │
│  │             Provider Adapter Layer                           │    │
│  │                                                              │    │
│  │ JoernAdapter LLVMAdapter SVFAdapter TraceAdapter LLMAdapter   │    │
│  └────────────────────────────┬─────────────────────────────────┘    │
│                               │                                      │
│                               ▼                                      │
│  ┌──────────────────────────────────────────────────────────────┐    │
│  │             VERITAS Canonical Program Graph — VPG            │    │
│  │                                                              │    │
│  │ Syntax │ CFG │ Symbols │ Calls │ Memory │ Def-Use │ Types     │    │
│  │ + Stable IDs + Provenance + Confidence + Assumptions          │    │
│  └───────────────────────┬──────────────────────────────────────┘    │
│                          │                                           │
│             ┌────────────┴───────────────┐                           │
│             ▼                            ▼                           │
│   ┌───────────────────┐       ┌─────────────────────┐                │
│   │ Function Summary  │       │   Evidence Engine   │                │
│   │       DB          │◄─────►│                     │                │
│   └─────────┬─────────┘       └──────────┬──────────┘                │
│             │                            │                           │
│             └────────────┬───────────────┘                           │
│                          ▼                                           │
│                 ┌─────────────────┐                                  │
│                 │   Evidence IR   │                                  │
│                 └────────┬────────┘                                  │
│                          ▼                                           │
│                 ┌─────────────────┐                                  │
│                 │ Review/Reasoning│                                  │
│                 │     Agents      │                                  │
│                 └─────────────────┘                                  │
└──────────────────────────────────────────────────────────────────────┘
```

The conceptual distinction is important:

**CPG describes program structure and relations. Evidence IR describes why VERITAS believes a security claim is true.**

Joern helps enormously with the former and supplies important atoms for the latter, but should not define the latter.

---

# 2. Do not directly bind VERITAS to Joern's database format

This is probably the most important implementation decision.

Current Joern 4.x moved its graph backend from OverflowDB to **FlatGraph**. The Joern project explicitly identifies the 4.0 transition as an OverflowDB→FlatGraph migration. ([GitHub][2])

Joern's supported exporter can export the entire graph using:

```text
joern-export --repr=all --format=neo4jcsv
joern-export --repr=all --format=graphml
joern-export --repr=all --format=graphson
joern-export --repr=all --format=dot
```

and can separately produce AST, CFG, CDG, DDG, PDG and CPG views. ([Joern Docs][3])

But I would **not** use any of those formats as VERITAS's permanent ingestion ABI either.

The architecture I recommend is:

```text
                      Joern compatibility boundary
                                │
                                ▼
Source ──► Joern Frontend ──► Joern CPG / FlatGraph
                                │
                                ▼
                    ┌──────────────────────┐
                    │ veritas-joern-export │
                    │ Scala / JVM sidecar  │
                    └──────────┬───────────┘
                               │
                        stable VERITAS ABI
                               │
                               ▼
                    ┌──────────────────────┐
                    │ VCPG Bundle          │
                    │ Arrow/Proto + manifest│
                    └──────────┬───────────┘
                               │
                               ▼
                       VERITAS Importer
```

The sidecar is allowed to be tightly coupled to Joern. The rest of VERITAS is not.

---

# 3. Define a VERITAS CPG Bundle

I would call the interchange format something like:

**VCPG — VERITAS Canonical Program Graph Bundle**

A bundle starts with a manifest:

```yaml
format: veritas.vcpg
version: 1

producer:
  name: joern
  joern_version: 4.x
  adapter_version: 0.1.0
  frontend: c2cpg
  cpg_schema_version: "..."
  schema_digest: sha256:...

source:
  repository: ...
  revision: ...
  root: ...
  language: C++

analysis:
  overlays:
    - base
    - controlflow
    - callgraph
    - ossdataflow

  configuration_digest: sha256:...

capabilities:
  ast: true
  cfg: true
  symbols: true
  types: true
  callgraph: true
  reaching_def: true
  control_dependence: true
  points_to: false

partitions:
  nodes: ...
  edges: ...
  entities: ...
```

For large projects, I would use:

**Manifest JSON/YAML + Apache Arrow/Parquet shards**

rather than GraphSON.

| Format          | VERITAS suitability                         |
| --------------- | ------------------------------------------- |
| GraphML         | Good debugging; poor at huge scale          |
| GraphSON        | Easy prototype; huge JSON overhead          |
| Neo4j CSV       | Reasonable bulk import, but stringly typed  |
| Joern FlatGraph | Excellent performance, terrible coupling    |
| Protobuf        | Excellent stable API                        |
| Arrow IPC       | Excellent streaming/bulk ingestion          |
| Parquet         | Excellent persistent/sharded representation |

My preferred combination would be:

```text
manifest.json

nodes/
  method.arrow
  call.arrow
  identifier.arrow
  control_structure.arrow
  ...

edges/
  ast.arrow
  cfg.arrow
  call.arrow
  ref.arrow
  reaching_def.arrow
  cdg.arrow
  ...

extensions/
  joern-specific.arrow
```

For the MVP, though, `joern-export --repr=all --format=graphson` is perfectly reasonable. Joern officially supports GraphSON for whole-graph export. ([Joern Docs][4])

---

# 4. Joern CPG → VERITAS VPG mapping

There should be **two layers**, not one:

```text
Joern CPG
   │
   ▼
Raw Joern Graph
   │
   │ lossless
   ▼
Joern Normalization
   │
   ▼
VERITAS VPG
```

The Raw layer is critical for auditability.

### Entity mapping

| Joern                  | VPG canonical concept |
| ---------------------- | --------------------- |
| `FILE`                 | `SourceFile`          |
| `NAMESPACE_BLOCK`      | `Namespace`           |
| `TYPE_DECL`            | `TypeDecl`            |
| `TYPE`                 | `Type`                |
| `MEMBER`               | `Field`               |
| `METHOD`               | `Function`            |
| `METHOD_PARAMETER_IN`  | `Parameter`           |
| `METHOD_PARAMETER_OUT` | `ParameterEffect`     |
| `METHOD_RETURN`        | `ReturnValue`         |
| `LOCAL`                | `LocalVariable`       |
| `IDENTIFIER`           | `VariableUse`         |
| `FIELD_IDENTIFIER`     | `FieldUse`            |
| `LITERAL`              | `Constant`            |
| `CALL`                 | `Operation/CallSite`  |
| `METHOD_REF`           | `FunctionReference`   |
| `CONTROL_STRUCTURE`    | `ControlNode`         |
| `RETURN`               | `ReturnOp`            |
| `BLOCK`                | `Block`               |

Joern's distinction between declarations, identifiers, calls and control-flow nodes is useful because CFG nodes are themselves AST nodes in the CPG specification. ([Code Property Graph][1])

### Relation mapping

| Joern relation   | VPG relation         |
| ---------------- | -------------------- |
| `AST`            | `syntax_child`       |
| `CFG`            | `control_flow`       |
| `CALL`           | `may_call/must_call` |
| `ARGUMENT`       | `call_argument`      |
| `RECEIVER`       | `call_receiver`      |
| `REF`            | `references_symbol`  |
| `REACHING_DEF`   | `def_use`            |
| `CDG`            | `control_dependency` |
| `DOMINATE`       | `dominates`          |
| `POST_DOMINATE`  | `post_dominates`     |
| `EVAL_TYPE`      | `has_type`           |
| `CONTAINS`       | `contains`           |
| `PARAMETER_LINK` | `parameter_in_out`   |

`REACHING_DEF` in particular should become a first-class evidence edge. In Joern it expresses that a variable definition reaches another program node without reassignment, and the edge carries the propagated variable name. ([Code Property Graph][1])

---

# 5. Normalize Joern's operator representation

This will be one of the adapter's most important jobs.

Joern frequently represents language operations as calls, conceptually along the lines of:

```text
<operator>.assignment
<operator>.addition
<operator>.fieldAccess
<operator>.indirection
...
```

VERITAS should normalize those into semantic operations:

```text
CALL "<operator>.assignment"
        │
        ▼
VPG.Assign

CALL "<operator>.fieldAccess"
        │
        ▼
VPG.FieldLoad / FieldAddress

CALL "<operator>.indirection"
        │
        ▼
VPG.Load / Dereference
```

Otherwise subsequent WPA rules become Joern-specific:

```text
bad:
if CALL.name == "<operator>.assignment" ...

good:
match VPG.Assign(dst, src)
```

Then LLVM could produce the exact same canonical relation:

```text
LLVM store
      │
      ▼
 VPG.Store
```

This is what allows:

```text
Joern ─────┐
LLVM ──────┼──► common VERITAS analysis
Binary ────┘
```

---

# 6. Stable IDs are more important than Joern node IDs

Never treat a Joern graph node ID as a persistent VERITAS identifier.

Instead define three different identifiers:

```text
ProviderNodeId
    Joern node 9374289
       │
       ▼
OccurrenceId
    exact occurrence in source
       │
       ▼
EntityId
    semantic program entity
```

For example:

```text
EntityId(Function):
hash(
    repository-id,
    language,
    namespace,
    class/type,
    function-name,
    normalized-signature
)
```

while an individual expression can use:

```text
OccurrenceId:
hash(
    file-id,
    enclosing-function-id,
    source-span,
    normalized-node-kind,
    structural-path
)
```

Keep:

```yaml
provider_identity:
  provider: joern
  snapshot: abc123
  node_id: 9176321
```

only as provenance.

This becomes essential for incremental Summary DB updates.

---

# 7. Provenance must be part of every fact

This is where VERITAS should go significantly beyond CPG.

Suppose Joern gives:

```text
A ──REACHING_DEF(x)──► B
```

Do not turn this simply into:

```text
DefUse(A, B, x)
```

Turn it into:

```text
DefUse {
    from: A
    to: B
    variable: x

    provenance {
        provider: JOERN
        provider_version: ...
        source_edge: ...
        frontend: c2cpg
        overlay: ossdataflow
    }

    semantics {
        kind: MAY
        scope: intraprocedural
    }

    confidence {
        type_resolution: ...
        call_resolution: ...
    }
}
```

That one decision will greatly improve VERITAS's ability to explain analysis results later.

---

# 8. Capture Joern assumptions, not only Joern conclusions

This is especially important for taint analysis.

Joern supports user-defined semantics for external functions. Without a model, Joern deliberately makes conservative assumptions about how external methods propagate taint, which can create additional possible flows. Custom semantics can specify exact argument-to-argument or argument-to-return propagation. ([Joern Docs][5])

VERITAS therefore needs to distinguish:

```text
DATAFLOW_EDGE
    provenance = joern

from:

DATAFLOW_EDGE
    provenance = joern
    assumption =
        external_call_semantics(foo)
```

For example:

```yaml
fact:
  kind: DataFlow
  src: E123
  dst: E918

derived_by:
  provider: joern
  analysis: ossdataflow

assumptions:
  - external_method_model:
      function: libc.foo
      model: conservative_passthrough

resolution:
  call_target: unresolved

confidence: MEDIUM
```

This is exactly the kind of information an LLM review agent needs.

---

# 9. This maps naturally into Evidence IR

The pipeline becomes:

```text
             Joern CPG
                │
                ▼
           CPG Adapter
                │
                ▼
         VERITAS VPG Facts
                │
       ┌────────┴────────┐
       ▼                 ▼
Summary Engine       WPA Engine
       │                 │
       └────────┬────────┘
                ▼
          Evidence Builder
                │
                ▼
            Evidence IR
```

For example, instead of giving an LLM:

> “There may be a taint problem at foo.c:138.”

VERITAS can construct:

```text
Claim
  attacker_controlled(input)
        reaches
  argument[1](exec)

Evidence
  E1 PARAMETER input
  E2 REF input → parameter
  E3 REACHING_DEF input → assignment
  E4 CFG assignment → call
  E5 REACHING_DEF assignment → exec.arg1

Control Constraints
  C1 call occurs under condition argc > 1

Call Resolution
  exec → libc.exec

Provenance
  E1-E5: Joern/ossdataflow

Assumptions
  exec semantics: VERITAS libc model

Cross Validation
  LLVM def-use: confirmed
  WPA alias analysis: confirmed

Confidence
  0.96
```

That illustrates the fundamental distinction:

```text
CPG
= "What relations exist?"

Evidence IR
= "What claim is being made,
   which relations support it,
   what analysis produced them,
   what assumptions were necessary,
   and how trustworthy are they?"
```

---

# 10. Joern becomes a strong bootstrap for the Function Summary DB

For each `METHOD`, the importer should trigger a `FunctionSummaryBuilder`.

Joern can provide the initial structural facts:

```text
                  Function Summary
                         │
       ┌─────────────────┼───────────────────┐
       │                 │                   │
    Interface          Behavior          Dependencies
       │                 │                   │
 parameters          data dependencies       callees
 return type         control predicates      callers
 types               field accesses          external APIs
 signature           reads/writes
                     allocations
                     calls
```

Then VERITAS enriches it:

```text
Joern
  ↓
syntax/cfg/def-use/calls

LLVM/SVF
  ↓
alias/points-to/memory effects

Symbolic Engine
  ↓
constraints/preconditions

Soufflé/WPA
  ↓
interprocedural fixed-point

LLM
  ↓
semantic role/API meaning

────────────────────────────
Function Summary DB
```

So Joern gives us an extremely useful **Level-0/Level-1 Summary**, but VERITAS remains responsible for the higher-precision WPA summary.

---

# 11. Don't try to make Joern solve all WPA problems

This is an important architectural boundary.

The CPG should not cause us to abandon the VERITAS plan for precise program analysis.

For difficult C/C++:

```text
pointers
aliasing
function pointers
virtual calls
templates
macro expansion
cross-TU state
heap objects
complex ownership
concurrency
```

Joern provides valuable evidence but shouldn't automatically become the authoritative oracle.

I'd define evidence precedence like:

```text
                Semantic precision

 LLVM/SVF memory model       ██████████████
 Clang semantic AST          █████████████
 Joern CPG                   ██████████
 fuzzy/source-only model     ███████
 LLM inference               ████

                              +
                             provenance
                              +
                           cross-validation
```

The LLM's semantic power is complementary rather than competing with those analyzers.

---

# 12. Incremental update design

This is another place where the adapter needs more than an `import()` function.

The CPG specification itself has an optional `HASH` property and explicitly notes that hashes can be used to determine whether code has already been analyzed in incremental pipelines. ([Code Property Graph][1])

But VERITAS should implement its own incremental identity.

Use:

```text
Git Change
    │
    ▼
Changed Files
    │
    ▼
Affected Functions/TUs
    │
    ▼
New Joern snapshot
    │
    ▼
Joern Adapter
    │
    ▼
Canonical Function Graph Hash
    │
  compare
    │
    ├──────── unchanged ───────► reuse Summary
    │
    └──────── changed
               │
               ▼
          Dirty Summary
               │
               ▼
        reverse call graph
               │
               ▼
        dependent summaries
```

Each function receives:

```text
syntax_hash
cfg_hash
call_hash
defuse_hash
type_dependency_hash

canonical_graph_hash =
    H(all above)
```

So changing:

```c
int f(int);
```

to:

```c
long f(long);
```

can invalidate callers even if their source text did not change.

This is far superior to just checking file timestamps.

---

# 13. Importer module structure

I would implement it roughly as:

```text
adapters/
└── joern/
    ├── exporter/
    │   ├── JoernGraphReader.scala
    │   ├── NodeExporter.scala
    │   ├── EdgeExporter.scala
    │   ├── OverlayInspector.scala
    │   └── BundleWriter.scala
    │
    ├── importer/
    │   ├── BundleReader.cpp
    │   ├── SchemaValidator.cpp
    │   ├── IdResolver.cpp
    │   ├── EntityNormalizer.cpp
    │   ├── OperatorNormalizer.cpp
    │   ├── TypeNormalizer.cpp
    │   ├── CallNormalizer.cpp
    │   ├── DataFlowNormalizer.cpp
    │   └── ProvenanceBuilder.cpp
    │
    ├── mappings/
    │   ├── nodes.yaml
    │   ├── edges.yaml
    │   ├── operators.yaml
    │   └── versions/
    │
    └── tests/
        ├── golden-c/
        ├── golden-cpp/
        ├── golden-java/
        └── schema-compat/
```

The exporter is Scala because it lives **inside Joern's compatibility boundary**.

The VERITAS importer can remain C++/Rust/whatever we choose for the VERITAS core.

---

# 14. Use capability negotiation

Joern has many frontends, with different maturity levels and different available semantic information. Its current documentation describes source, bytecode and binary frontends across C/C++, Java, JavaScript, Python, Go, JVM bytecode, x86/x64 and others. ([Joern Docs][6])

Therefore don't assume:

```text
Joern CPG == same information everywhere
```

Export:

```yaml
capabilities:
  AST: COMPLETE
  CFG: COMPLETE

  types:
    level: PARTIAL
    unresolved: 137

  call_resolution:
    static: COMPLETE
    dynamic: PARTIAL

  reaching_def:
    available: true

  external_semantics:
    coverage: 0.74
```

Evidence IR can then incorporate capability quality into confidence.

---

# 15. Preserve unknown Joern extensions

Joern is explicitly designed to be extensible via additional CPG layers/passes. ([Joern Docs][7])

So this would be a mistake:

```cpp
switch(label) {
   case METHOD:
   case CALL:
   ...
   default:
       drop();
}
```

Instead:

```text
Known Joern construct
       ↓
Canonical VPG construct

Unknown/extension construct
       ↓
ProviderExtension {
    provider = JOERN
    label = ...
    properties = ...
}
```

Then future Joern tags, findings or custom passes can be used without changing the importer immediately.

---

# 16. One especially useful extension: Joern-generated slices

Joern also provides `joern-slice`, including backwards data-flow slices and variable-usage slices, with JSON intended for ingestion by other tools. ([GitHub][8])

I wouldn't use slices as the core graph importer, but they are useful as an **Evidence Accelerator**:

```text
Full CPG
   │
   ├──────────► VPG
   │
   └─ joern-slice
           │
           ▼
      Candidate Evidence
           │
           ▼
      VERITAS validates
```

For example:

```text
sink = memcpy()
       │
       ▼
Joern backward slice
       │
       ▼
20 candidate nodes
       │
       ▼
VERITAS WPA + Summary DB
       │
       ▼
precise 7-node Evidence IR
```

That can drastically reduce the search space presented to the expensive reasoning layer.

---

# 17. Production interface I'd actually implement

The external API could be very small:

```text
veritas-joern-export \
    --input ./cpg.bin \
    --output ./foo.vcpg \
    --include ast,cfg,call,ref,ddg,cdg,type \
    --partition function
```

then:

```text
veritas import \
    --provider joern \
    ./foo.vcpg
```

And source-driven:

```text
veritas analyze \
    --provider joern \
    --source ./project \
    --revision HEAD
```

Internally:

```cpp
class ProgramGraphProvider {
public:
    virtual ProviderManifest manifest() = 0;

    virtual Stream<Node> nodes() = 0;
    virtual Stream<Edge> edges() = 0;

    virtual CapabilitySet capabilities() = 0;
};
```

Then:

```cpp
class JoernProvider : public ProgramGraphProvider {};
class LLVMProvider : public ProgramGraphProvider {};
class SVFProvider   : public ProgramGraphProvider {};
class TraceProvider : public ProgramGraphProvider {};
```

This is the abstraction I would build the whole project around.

---

# 18. The resulting VERITAS architecture becomes quite powerful

The project can evolve into this model:

```text
                       ┌──────────────────┐
                       │      Source      │
                       └────────┬─────────┘
                                │
           ┌────────────────────┼─────────────────────┐
           ▼                    ▼                     ▼
       Joern CPG            LLVM/Clang            Binary
           │                    │                     │
           ▼                    ▼                     ▼
      Joern Adapter        LLVM Adapter          Ghidra/etc.
           │                    │                     │
           └────────────────────┼─────────────────────┘
                                ▼
                 ┌──────────────────────────┐
                 │ VERITAS Program Graph   │
                 │         VPG              │
                 │                          │
                 │ canonical identities     │
                 │ relations                │
                 │ provenance               │
                 │ uncertainty              │
                 └────────────┬─────────────┘
                              │
                      symbolic facts
                              ▼
        ┌─────────────────────┴─────────────────────┐
        │                                           │
        ▼                                           ▼
┌───────────────────┐                     ┌───────────────────┐
│ WPA / Soufflé     │                     │ Summary Engine    │
│                   │◄───────────────────►│                   │
│ alias/taint/state │                     │ Function Summary  │
└──────────┬────────┘                     └──────────┬────────┘
           │                                         │
           └──────────────────┬──────────────────────┘
                              ▼
                    ┌───────────────────┐
                    │   Evidence IR     │
                    │                   │
                    │ Claim             │
                    │ Evidence          │
                    │ Constraints       │
                    │ Provenance        │
                    │ Assumptions       │
                    │ Confidence        │
                    └─────────┬─────────┘
                              ▼
                  LLM Security Review Agent
```

And this resolves a subtle design issue in VERITAS:

> **VPG is the machine-analysis IR. Evidence IR is the reasoning/audit IR. Function Summary DB is the scalable interprocedural abstraction.**

They are three different things and should remain three different things.

---

# 19. Recommended implementation sequence

I would build it in six increments:

1. **Joern Adapter MVP:** C/C++, `METHOD/CALL/PARAM/LOCAL/IDENTIFIER`, `AST/CFG/CALL/REF`, GraphSON input, stable VPG IDs.
2. **Data-flow:** import `REACHING_DEF`, `CDG`, arguments/receivers and Joern data-flow assumptions; Joern requires its `ossdataflow` layer to be calculated for those data-flow representations. ([Joern Docs][3])
3. **Native sidecar:** replace GraphSON hot path with `veritas-joern-export`, producing Arrow/Protobuf VCPG bundles.
4. **Summary integration:** compute per-function canonical hashes and feed the incremental Summary DB.
5. **Evidence IR integration:** every Evidence IR atom can reference a VPG node/edge and carry the complete provider provenance chain.
6. **Multi-provider validation:** add Clang/LLVM/SVF/Soufflé and make Joern facts corroboratable rather than authoritative.

---

## The architectural idea I'd lock down now

I would make the following abstraction a core VERITAS principle:

```text
             Analyzer-specific world
────────────────────────────────────────────────────

 Joern CPG      LLVM IR      SVF      Trace      LLM
     │             │          │         │         │
     ▼             ▼          ▼         ▼         ▼
  Provider       Provider    Provider  Provider  Provider
  Adapter        Adapter     Adapter   Adapter   Adapter

──────────────────── Evidence Boundary ─────────────────

                      VPG
             VERITAS Program Graph
                       │
             canonical program facts
                       │
          ┌────────────┴────────────┐
          ▼                         ▼
      Summary DB               WPA Engine
          │                         │
          └────────────┬────────────┘
                       ▼
                  Evidence IR
                       │
                       ▼
                 Review Agent
```

The **Evidence Boundary** is the key architectural concept.

Below it, VERITAS never says:

> “Joern says there is a vulnerability.”

It says:

> “Joern produced evidence E1–E7 under assumptions A1–A3; LLVM corroborated E3/E5; WPA derived F1 from those facts; Summary S21 propagated the effect across four calls; therefore Evidence IR supports claim C with this explicit derivation chain.”

That is, in my view, the architecture that can make **VERITAS materially different from both conventional static analyzers and LLM-only code-review agents**. Joern gives us an excellent starting CPG and query ecosystem, while the canonical VPG + Summary DB + Evidence IR gives us the infrastructure for high-precision, explainable, incremental whole-program security reasoning. ([Joern Docs][7])

[1]: https://cpg.joern.io/?utm_source=chatgpt.com "Code Property Graph Specification Website | Code Property Graph Specification Website"
[2]: https://github.com/joernio/joern?utm_source=chatgpt.com "GitHub - joernio/joern: Open-source code analysis platform for C/C++/Java/Binary/Javascript/Python/Kotlin based on code property graphs. Discord https://discord.gg/vv4MH284Hc · GitHub"
[3]: https://docs.joern.io/export/ "Exporting Graphs | Joern Documentation"
[4]: https://docs.joern.io/export/?utm_source=chatgpt.com "Exporting Graphs | Joern Documentation"
[5]: https://docs.joern.io/dataflow-semantics/?utm_source=chatgpt.com "Custom Data-Flow Semantics | Joern Documentation"
[6]: https://docs.joern.io/?utm_source=chatgpt.com "Overview | Joern Documentation"
[7]: https://docs.joern.io/code-property-graph/?utm_source=chatgpt.com "Code Property Graph | Joern Documentation"
[8]: https://github.com/joernio/joern/blob/master/joern-cli/JOERN_SLICE.md?utm_source=chatgpt.com "joern/joern-cli/JOERN_SLICE.md at master · joernio/joern · GitHub"
