# VERITAS review runtime foundation

RDX05 (#159) delivers the backend v2 wire models and native snapshot library.
RDX06 (#160) adds the structural index and narrow compiler-local provider.
This directory currently contains the schema, frozen Python types and tests.
The subprocess backend, controller, replay and host adapter
are later deliveries; this is not an installable review plugin yet. The native
SVF/Soufflé analysis path and `eir.v1` remain unchanged.

## Wire contract

`decode_v2(bytes, expected_request_id=..., expected_operation=...)` accepts one
bounded UTF-8 JSON envelope. It rejects unknown/duplicate fields, floats,
booleans in integer slots, overflows, invalid Unicode and mismatched responses.
Initial capture accepts operator selection keys, never a model path or a
post-capture artifact handle. Local bindings have `native_binding=None`.

Review artifact IDs use separate tagged kinds; they cannot replace native
FactIDs, AnalysisRunIDs or evidence IDs. Semantic IDs hash
`veritas.review.<kind>.v1` followed by NUL and canonical JSON. Audit timing is
passed separately and excluded. Native and Python tests pin identical Unicode,
control/NUL, empty-array and signed/unsigned boundary bytes and hash vectors.
The schema currently covers handshake, capture, context, seed-candidates,
local-evidence, native-evidence and release shapes. Verification codecs remain
unavailable until the qualification contract delivers their closed result and
budget definitions; `verify` is rejected. No operation executes here.

## Trusted native snapshot API

`CaptureReviewSnapshot(SnapshotRequest)` captures selected tracked/index/base
paths, including dirty files and deletions, plus explicitly selected untracked
files and trusted artifact selections. An empty tracked selection means every
index/base path. `selected_artifacts` may name files or directories for build,
include, generated, test or documentation inputs. `compile_commands_path` and
`program_manifest_path` are trusted operator selections, resolved before model
interaction. These native paths are not wire parameters.

Capture reads through directory descriptors with `O_NOFOLLOW`, refuses symlinks
(including base symlinks), rejects traversal and `.git` selections, and checks
bytes/inode/ctime, directory membership, base and index again before publishing.
Input changes return a retryable `FailedPrecondition`; missing inputs retain
explicit unavailable/frontier entries. File/byte limits are positive and
refusal is atomic. Git runs fixed argv with bounded output/deadline, no shell,
scrubbed configuration and disabled hooks/fsmonitor. No compiler is executed.

`Read`, `ReadBase` and `Context` use captured content-addressed blobs after live
edits. Context ranges are UTF-8 byte offsets and source/test/doc artifacts have
typed context IDs and explicit omitted byte ranges; no entity is invented. `Materialize` creates a new private,
read-only tree from those blobs and refuses existing/symlink destinations.
`LiveTreeAdvanced` reports subsequent selected-content, artifact-directory membership or Git-discovery changes.
The blobs live with the immutable snapshot handle; durable backend lifecycle
and enforcement of compiler/replay isolation belong to later tasks.

The repository identity is supplied by trusted invocation configuration. A local
base is recorded as `git:<commit-oid>` rather than minting a native revision or
analysis-run identity. Existing native revision bindings remain readable.
Recognized compile-command path operands use a structured repository-relative
identity; literal arguments and macro values stay exact and cannot alias a path
placeholder. Source/blob bytes stay exact. This does not remove the #125 portability
gate for native identities or authorize cross-host reuse.

## Program membership

A selected `veritas.review.program.v1` manifest has this closed form:

```json
{
  "version": "veritas.review.program.v1",
  "targets": [
    {"key":"app","kind":"EXECUTABLE","translation_units":["src/main.cc"],"dependencies":["shared"],"artifacts":[]},
    {"key":"shared","kind":"SHARED_LIBRARY","translation_units":["src/lib.cc"],"dependencies":[],"artifacts":["build/libshared.so"]}
  ]
}
```

Select `program_target_key` in trusted configuration. Allowed target kinds are
EXECUTABLE, STATIC_LIBRARY, SHARED_LIBRARY and TEST. Resolve dependencies
transitively, including shared libraries; selected dependency artifacts must
also be captured. A trusted build/link adapter can supply the same resolved
graph with `membership_source=BUILD_LINK_ARTIFACT`. Explicit declarations use
EXPLICIT_DECLARATION. DECLARED_CLOSED always records the operator declaration
as an assumption, never a discovered whole-program proof. Unknown targets,
missing dependency artifacts and uncaptured TUs preserve UNRESOLVED frontiers.
A compilation database without a declaration supplies only OUTPUT_HINT/OPEN
membership; its object output directories cannot establish closure.

Capture alone continues to report compiler capability as unqualified. The
separate structural provider validates commands and parses the captured include
closure; its own coverage describes the resulting capability. Missing inputs
remain visible. Capture makes no analytical/proof claim.

## Structural index and local evidence (RDX06)

`BuildStructuralIndex(snapshot, StructuralBudget)` uses the linked Clang library
in a bounded child process, with an in-memory filesystem containing captured
bytes only and no real-filesystem fallback. Project executables, wrappers,
plugins, linker, SVF and WPA are never invoked. Clang's default include paths
are disabled; headers must have been captured. Missing headers or compiler
errors yield an incomplete index with a frontier, and recovery ASTs emit no
local evidence. Selected program TU membership is respected; output hints stay
OPEN. No hard portable RSS isolation or replay authority is advertised here.

Compiler arguments are admitted with known arities: recognized Clang driver
syntax, the listed C/C++ standards, basic optimization/diagnostic switches,
macro definitions, captured include inputs and syntax-only output removal.
Unknown semantic flags, wrappers, plugins, modules/PCH and external include
paths refuse before compiler dispatch. Captured GNU response files have a
64-KiB aggregate byte ceiling, 4096 argument limit and depth limit four; their
expanded flags pass through the same admission. The initial ABI is explicitly
`x86_64-unknown-linux-gnu`; other ABIs are unsupported. Date/time predefined
macros use `SOURCE_DATE_EPOCH=0` in the scrubbed child environment.

The index retains function definitions, array/type descriptors, direct versus
indirect callsites, per-site operand indices and UTF-8 source byte anchors.
`StructuralIndex::Context` advertises only entities contained in a captured
source span. Index and evidence IDs exclude timing and absolute checkout paths.
`Usage` includes cold setup wall/CPU and compiler child CPU/peak RSS; index setup
is a distinct cost from warm investigation and must be charged once by the
future backend/controller. Wall, input bytes, TU/entity counts, pipe output and
observed child RSS are bounded. RSS is sampled at up to 25-ms intervals and
checked again using `wait4` peak RSS; sampling and termination can overshoot.
Worker failure/limit exhaustion refuses atomically and publishes no index.

`InvestigateLocal(LocalRequest)` takes a trusted native index handle alongside
the v2 request fields; that handle is never a wire/model field. It produces
schema-valid, separately identified `local-evidence.v1` observations for
`copy_bound.v1`, with no native analysis-run binding and no EIR/FactID conversion.
Registered copies are compiler-recognized global `memcpy`/`__builtin_memcpy`
with the default C ABI, no implementation/assembler override, and three exact
operands. Extents use compiler layout bytes for direct local constant integer
arrays (including multi-byte/wchar arrays). Pointer, VLA, heap, cast, macro-copy,
indirect and unsupported ABI forms leave absent slots and coverage reasons.
Captured constant macros used in array declarations are resolved by Clang.

Guard observations initially cover `unsigned_parameter <= sizeof(destination)`
or the exact literal byte capacity, and the corresponding `>` false branch.
Only an enclosing structured branch establishes `dominates=true`. A sibling
comparison is retained with `dominates=false`. Mutation, address/reference
escape, loops/goto/switch/lambda/inline assembly and unmodeled guards remain
unsupported; early-return guard inference is not implemented. These are local
observations, with no feasibility or safety verdict. Native domain producers
under #124 and the production review/replay release gate remain pending.

## Verification

```sh
python3 -m unittest discover -s plugins/veritas-evidence-review/tests -p test_review_types.py
cmake --build --preset homebrew-clang
ctest --test-dir build-homebrew-clang -R 'ReviewTypes|SnapshotCapture|LocalEvidence' --output-on-failure
```

The Python package uses only Python 3.11+ standard-library modules. All new
first-party code uses the repository's Apache-2.0 license notice; raw JSON is
covered by this directory's Apache-2.0 attribution and the repository LICENSE.
