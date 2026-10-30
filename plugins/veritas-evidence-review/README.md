# VERITAS review runtime foundation

RDX05 (#159) delivers the backend v2 wire models and native snapshot library.
RDX06 (#160) adds the structural index and narrow compiler-local provider.
This directory contains the schema, frozen Python types, bounded transport and tests.
RDX07 (#161) adds the public resident subprocess backend and exact-obligation
authority boundary. The controller, real replay and host adapter
remain later deliveries; this is not an installable review plugin yet. The native
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
local-evidence, native-evidence and release shapes. RDX07 closes the verification request/result, concrete-input domain and positive
verification budget codecs. A schema-valid verifier response alone has no authority.
The backend implements capture/context/seed/local/release; native/verify are
recognized and explicitly UNSUPPORTED until a qualified provider is configured.

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
source span. Its ID commits to the source-context premise, exact index and
advertised bindings; source-only provenance context IDs remain separate. Index
and evidence IDs exclude timing and absolute checkout paths.
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

## Public backend and authority boundary (RDX07)

Build `veritas-review-backend` and import `veritas_review.backend` from this
package's `scripts/` directory. The transport uses only the Python standard
library. The native service does not link or open SummaryDB for local mode.
All eight named subcommands exist. Begin with handshake, then capture; the
backend advertises the exact operations currently available. It never selects
practice mode. Native evidence has no configured adapter; production verification
has no registered producer. Both operations return UNSUPPORTED. This delivery
does not qualify replay, the controller, the plugin host, or a real review release.

A resident process keeps one immutable snapshot/index and its advertised context
handles until release or process exit. Each request and response is exactly one
bounded v2 JSON envelope followed by a newline; EOF also terminates a one-shot
request. Newlines inside strings use normal JSON escaping. A named subcommand
accepts its matching bootstrap request or an initial handshake; later requests
are dispatched by their closed operation envelope. A v1-only client cannot
capture v2 inputs. Duplicate request IDs refuse before execution; durable retry
receipts and aggregate model-turn admission belong to RDX08.

The backend consumes a trusted operator JSON file selected in argv, never a path
from a model request. All top-level fields below are required; selection maps
can be empty. Compile-command absolute paths must match the canonical project
root. The manifest map selects both a captured manifest path and its target.
`target_ref` currently accepts null for the captured working tree; revision-target
capture is unavailable. Source contexts come from captured compiler inputs;
TEST/DOC selections resolve only through `context_selection_keys`.

```json
{
  "project_key": "app",
  "project_root": "/absolute/canonical/project",
  "repository_id": "repo:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "base_ref": "HEAD",
  "compile_commands_path": "compile_commands.json",
  "selected_tracked": [],
  "selected_artifacts": ["include"],
  "allowed_untracked": [],
  "context_selections": {"design": {"path": "docs/design.md", "kind": "DOC"}},
  "programs": {"app": {"path": "review-program.json", "target": "app"}},
  "limits": {"wall_ms": 120000, "output_bytes": 1048576, "examined": 10000,
             "context_bytes": 32768, "request_bytes": 1048576, "requests": 256}
}
```

```python
from veritas_review.backend import Backend

with Backend(("/absolute/build/bin/veritas-review-backend", "handshake",
              "--config", "/absolute/operator.json"), cwd="/private/tmp") as backend:
    backend.handshake()
    captured = backend.call("capture", {
        "project_key": "app", "base_ref": "HEAD", "target_ref": None,
        "selected_untracked": [], "program_manifest_key": "app",
        "context_selection_keys": ["design"],
    }).result
```

Context returns only advertised handles and conservatively charges one token per
UTF-8 byte. Large source/diff artifacts outside the configured window are omitted
with incomplete coverage. DIFF artifacts are deterministic full-file replacements
of base and captured bytes; they do not invent entity bindings. Seeds name actual
advertised copy callsites in changed files. They are candidates, never findings.
Structural entity/TU work exhaustion refuses capture before publication; parse
and include gaps remain explicit partial coverage. Local evidence and its source
provenance retain their separate review IDs and nullable native binding.

`Backend.call` validates operations, parameters, advertised capabilities and the
pinned binding before dispatch. It uses trusted argv, a scrubbed environment,
a private cwd, nonblocking bounded stdin/stdout/stderr, deadlines, process groups,
and descendant process/RSS sampling at most every 250 ms. Cleanup covers the
dedicated POSIX session, including the compiler's separate process group; it does
not rely on macOS's redacted `ps sess` column. The OS sampler has its own bounded
output and deadline. Per-call and aggregate
wall/RSS allowances apply to the resident process and its children. Its
`transport_usage`/`last_usage` records cold startup wall time, sampled peak worker
RSS, output, sample count and deadline overshoot even on failure. The structural
provider also bounds and meters its compiler child. RSS limits are observed,
with sampling/termination overshoot; there is no portable hard isolation claim.
Requiring hard isolation returns UNSUPPORTED before launch. Malformed/mismatched
responses, floods and timeout kill the group and invalidate the session. A valid
refusal keeps the pinned snapshot available for a corrected request.

`VerificationPolicy` is trusted host state, never decoded from a model request.
Producer name/version/config registration alone cannot decide a result. The
policy must also admit the independently checked immutable result ID for the
exact obligation, request digest, binding, operands, domain, assumptions and
content-checked input/witness dependencies. Input evidence cannot serve as its
own proof. Timeout, unsupported, cancellation, errors and incomplete results
cannot promote a disposition. A concrete PROVED result means only
NO_DEFECT_OBSERVED for that input; no formal domain or universal-safe producer is
registered. Local report transitions never write authoritative native EIR state.
The test-bound-check fixture is a test-only authority vector, not a production
verifier or an analytical replay qualification.
