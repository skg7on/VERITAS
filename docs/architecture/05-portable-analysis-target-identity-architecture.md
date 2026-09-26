# Portable Analysis Target and Identity Architecture

Status: proposed
Tracking issue: [#125](https://github.com/skg7on/VERITAS/issues/125)

## 1. Purpose

VERITAS content addresses must be reproducible when the same source is analyzed
with the same declared semantics. Today `FunctionVariantID` hashes LLVM's
emitted `target-features` function attribute. Clang may populate that attribute
from the analysis host, so otherwise equivalent analyses can produce different
function, value, memory, fact, CPG, and evidence identities.

This architecture makes the analysis target an explicit, canonical input to the
pipeline. It preserves existing projects that omit target flags, while limiting
the cross-host portability promise to analyses whose resolved target is equal.

## 2. Decision Summary

1. Introduce a versioned `AnalysisTarget` value containing a normalized target
   triple, CPU, ABI, enabled and disabled feature sets, and provenance.
2. Resolve one project-level target during manifest ingestion. A target named by
   the compilation database is `explicit`; an omitted target is resolved from
   the current analysis host and marked `inferred`.
3. Inject the resolved target into both in-process Clang paths so AST extraction
   and LLVM IR generation cannot independently consult host defaults.
4. Include the canonical target in `BuildVariantID` under
   `veritas.build_variant.v2`.
5. Derive `FunctionVariantID` under `veritas.function-variant.v2` from the
   function symbol, v2 build variant, and calling convention. LLVM's emitted
   `target-features` string is not an identity input.
6. Migrate SummaryDB additively. Existing v1 rows remain readable; new analyses
   publish v2 identities and naturally recompute all dependent artifacts.

## 3. Required Invariants

- Two analyses with equal source inputs, normalized compile options, compiler
  identity, and explicit `AnalysisTarget` produce equal content addresses on
  supported hosts.
- Distinct triples, CPUs, ABIs, or semantic feature sets produce distinct
  `BuildVariantID`s.
- An inferred target is never presented as portable merely because its string
  happens to match an explicit target.
- Both Clang stages consume exactly the target recorded in the manifest.
- LLVM attributes, linked-module state, target registration, host OS, and host
  CPU detection are observations, never sources of target identity.
- Source-declared function specialization remains represented by source and
  symbol identity; emitted function bodies remain represented by
  `FunctionBodyID`. Host-added attributes do not create a function variant.
- Old stores are not rewritten in place and old stable IDs never acquire new
  meaning.

## 4. Canonical Analysis Target

The build layer owns this value:

```cpp
enum class TargetProvenance {
  kExplicit,
  kInferred,
};

struct AnalysisTarget {
  std::string schema_version;      // "analysis-target.v1"
  std::string triple;              // LLVM-normalized triple
  std::string cpu;                 // canonical CPU or "generic"
  std::string abi;                 // empty only when the target has no ABI selector
  std::vector<std::string> enabled_features;
  std::vector<std::string> disabled_features;
  TargetProvenance provenance;
};
```

Feature names are stored without `+` or `-`, lower-cased where LLVM defines
them case-insensitively, sorted, and unique. A feature cannot occur in both
sets. The canonical encoding is length-prefixed and field ordered; it does not
use JSON, LLVM printing, locale-sensitive formatting, or container iteration
order.

`provenance` participates in the target digest. Thus an implicit host-derived
analysis does not cache-hit an explicitly declared portable analysis even if
their effective machine settings coincide.

## 5. Resolution Rules

The manifest loader parses target-affecting driver options from every normalized
translation-unit command. Recognized spellings include joined and separated
target triples and the supported CPU, architecture, ABI, and feature flags.
Parsing is centralized; identity code and Clang argument construction do not
reparse command strings independently.

Resolution proceeds as follows:

1. Normalize all explicitly declared target fields.
2. Require every translation unit to resolve to the same project-level target.
   Conflicts fail before AST or IR work and report the differing source files
   and canonical targets.
3. Fill omitted fields with target-specific portable baselines. An explicit
   triple without a CPU does not inherit the analysis host CPU; it uses the
   declared triple's generic baseline.
4. If no target field is declared, resolve the analysis host target once, mark
   it `inferred`, and record the complete result. This preserves today's
   command surface while honestly scoping the result to that resolved target.
5. Reject unknown target-affecting spellings rather than silently omitting them
   from identity while still forwarding them to Clang.

The initial baseline table covers the triples supported by CI and the canonical
development configuration. Adding a new target family requires a reviewed
baseline entry and tests; it must not fall back to LLVM host detection.

## 6. Compiler Invocation Contract

`TranslationUnitCommand` carries the resolved target, or an immutable reference
to the manifest target. `CompileFlags` produces one canonical target argument
sequence and removes equivalent target spellings already present in the raw
command.

Both consumers use that same sequence:

- `frontend::clang::ProjectAstExtractor` for AST extraction;
- `analysis::llvm::ProjectIrBuilder` for LLVM IR generation.

After IR emission, VERITAS validates the module triple and each applicable
target attribute against the declared target. A mismatch is an analysis error,
not a reason to change identity. This validation detects a compiler invocation
that ignored or contradicted the manifest without making emitted attributes an
identity source.

## 7. Identity Model v2

### 7.1 Build variant

`veritas.build_variant.v2` hashes:

```text
compiler_id
compile_options_hash_without_duplicate_target_spellings
canonical_analysis_target
```

The canonical target is therefore the authoritative target dimension. The
separate persisted fields remain queryable metadata, but identity has only one
encoding of the target.

### 7.2 Function variant

`veritas.function-variant.v2` hashes:

```text
FunctionSymbolID
BuildVariantID(v2)
calling_convention
```

It does not hash `llvm::Function::getFnAttribute("target-features")`. A
source-declared target attribute is already within the revision/source identity,
and any emitted semantic body difference is captured by `FunctionBodyID`.
Function multiversioning that creates distinct symbols remains distinct at
`FunctionSymbolID`.

### 7.3 Downstream identities

No downstream schema needs a special host-normalization rule. Value references,
abstract objects, memory references, summaries, CPG nodes, facts, witnesses,
and evidence continue deriving from their owning stable IDs. Replacing the
tainted root with v2 identities repairs the chain by construction.

## 8. Persistence and Migration

The manifest canonical form gains `analysis_target.v1`. SummaryDB advances by
one schema version and adds target schema, CPU, ABI, feature encoding, and
provenance columns to `build_variants`.

Migration is additive:

- existing build-variant rows remain valid legacy records;
- new rows require the complete v1 target fields;
- v1 and v2 stable IDs cannot collide because their domain tags differ;
- no existing CAS object, summary, fact, SCC state, or evidence record is
  rewritten;
- the first v2 analysis misses v1 caches and recomputes dependent artifacts;
- subsequent equal v2 analyses regain normal cache reuse.

Readers that expose build metadata report legacy target information as
`legacy/unknown`, never as explicit or portable.

## 9. External IR Boundary

M11's bitcode/LLVM IR adapter must not recreate the bug by trusting the host.
It obtains an `AnalysisTarget` from module-declared target data and explicit
adapter configuration. Missing or contradictory target data lowers input
fidelity or fails according to the M11 contract; it never consults the current
host to complete an externally supplied module silently.

Native-source and external-IR ingestion share the canonical encoder and v2
identity constructors.

## 10. Verification Strategy

### Unit tests

- All supported target-flag spellings canonicalize identically.
- Feature order and duplicates do not affect the target digest.
- Enable/disable conflicts and mixed-TU targets fail with stable diagnostics.
- Explicit and inferred provenance produce distinct target and build IDs.
- Distinct semantic targets produce distinct v2 build and function variants.
- Changing only an emitted host `target-features` string does not change
  `FunctionVariantID`.
- SummaryDB upgrades preserve v1 rows and accept complete v2 rows.

### Integration tests

- AST and IR ClangTool paths receive the same canonical target arguments.
- Two independent materializations with the same explicit target but different
  simulated host defaults produce identical function/value/memory IDs,
  summaries, CPG projection, facts, and evidence content addresses.
- An omitted target records an inferred target and different inferred targets
  produce different build variants.
- A module that contradicts its manifest target is rejected before publication.

### Golden policy

Once the cross-host qualification passes, evidence goldens stop masking IDs
whose only instability was issue #125. IDs derived from
`engine_toolchain_identity` remain excluded or masked until the independent
Souffle reproducibility problem is solved.

## 11. Acceptance Criteria

- The reproducer from issue #125 yields identical non-run content addresses on
  two hosts when both analyses declare the same target.
- The same source under different declared targets yields different
  `BuildVariantID`, `FunctionVariantID`, and dependent identities.
- No production identity constructor reads LLVM `target-features`.
- Native AST and IR stages are proven to consume the manifest target.
- Existing v1 stores open and remain queryable without destructive migration.
- Full build, CTest suite, qualification labels, M9 entry gate, formatting,
  license-header, and no-skip checks pass.

## 12. Rejected Alternatives

### Remove `target-features` only

This fixes the immediate leak but leaves target triple and CPU semantics outside
the authoritative build identity and allows the two Clang stages to diverge.

### Canonicalize LLVM's emitted attribute

Sorting the string removes formatting instability, not host-derived semantics.
The pipeline would still trust output affected by the environment it is trying
to exclude.

### Require an explicit target for every analysis

This is portable but unnecessarily breaks existing compilation databases.
Recording a complete inferred target preserves compatibility without making a
false portability promise.

## 13. Implementation Boundaries

This work changes target resolution, manifest serialization, Clang invocation,
identity domain versions, SummaryDB metadata migration, and portability tests.
It does not attempt to make Souffle binaries reproducible, merge stores built
for different targets, or preserve cache hits between v1 and v2 identities.
