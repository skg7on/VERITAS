# Tutorial: Translate Defect Checks into EIR Code Reviews

This tutorial works through five end-to-end code-review cases using the
Evidence IR implementation and project fixtures in the current tree. Each case
starts with a concrete C++ shape, builds a real SummaryDB, emits the current
M10B handoff and M10C EIR, and ends with the strongest review conclusion that
the evidence supports.

The examples intentionally include implementation limits. They show how to
avoid turning a missing producer, an unmodeled call, or a truncated query into
a stronger claim than VERITAS can justify.

## 1. The translation discipline

A source-level defect check becomes an EIR-backed review in six steps:

1. State one defect predicate, such as `value(copy_size) > capacity(dst)`.
2. Bind the predicate to stable source, sink, and memory-object identities.
3. Run bounded semantic queries for flow, range, capacity, aliasing, checks,
   unknowns, and provenance.
4. Preserve each query's completeness instead of treating an empty result as
   false.
5. Assemble and validate one immutable Evidence case.
6. Write a review conclusion that cites only represented evidence and names
   any missing proof.

For the registered overflow query, the current real pipeline provides flow,
query-completion facts, a scoped dominating-check absence certificate, and
provenance. It does not yet produce value-range, destination-capacity,
queryable alias, or positive dominating-check facts. Every case below keeps
those slots empty instead of reconstructing them from source text.

Use the public outputs for different jobs:

| Output | Purpose in this tutorial |
| --- | --- |
| `--format json` | Inspect the complete M10B `EvidenceBuildInput`, including per-query completeness and truncation. This is not EIR. |
| `--format eir-t --level l1` | Read the validated M10C case and review its claims, facts, provenance, dependencies, and state. |
| `--format eir-json --level l1` | Feed the full case to deterministic diagnostics or presentation code. It has no reader in M10C. |
| `--format protobuf --level l1` | Use the lossless machine interchange and decode boundary. |

The IDs in the examples are content-addressed and will differ across compiler
hosts. Match semantic members such as `reachable`, `query_completion`, and
`dominates_bounds_check`; do not copy IDs from this guide.

## 2. Prepare the runnable fixture databases

Build VERITAS first:

```bash
cmake --preset default
cmake --build --preset default
```

The checked-in fixture compilation databases contain an `@PROJECT_ROOT@`
token. The integration harness copies each fixture to a temporary directory
and substitutes that token before analysis. Reproduce the same materialization
from the repository root:

```bash
repo_root="$PWD"
case_root="$(mktemp -d "${TMPDIR:-/tmp}/veritas-eir-cases.XXXXXX")"

materialize_fixture() {
  local fixture="$1"
  local project="$case_root/${fixture}-project"
  local store="$case_root/${fixture}-summarydb"

  cp -R "$repo_root/tests/fixtures/projects/$fixture" "$project"
  python3 - "$project" <<'PY'
from pathlib import Path
import sys

project = Path(sys.argv[1]).resolve()
database = project / "compile_commands.json"
database.write_text(
    database.read_text().replace("@PROJECT_ROOT@", str(project))
)
PY
  "$repo_root/build/bin/veritas-build" analyze \
    --project "$project" \
    --output "$store"
}

for fixture in \
  evidence_overflow_unsafe \
  evidence_overflow_safe \
  evidence_overflow_opaque_validator \
  evidence_overflow_summary
do
  materialize_fixture "$fixture"
done

unsafe_db="$case_root/evidence_overflow_unsafe-summarydb"
safe_db="$case_root/evidence_overflow_safe-summarydb"
opaque_db="$case_root/evidence_overflow_opaque_validator-summarydb"
summary_db="$case_root/evidence_overflow_summary-summarydb"
```

Each database contains exactly one current native CPG projection and one
current fact run, which is the public Evidence command's required input shape.

## 3. Use case: direct unchecked copy

### 3.1 Defect check

The unsafe fixture passes a packet-controlled length directly to `memcpy`:

```cpp
void copy_payload(Packet* p, Buffer* b) {
  memcpy(b->data, p->payload, p->length);
}
```

Translate the check into one claim and its proof questions:

| Review concept | EIR translation |
| --- | --- |
| Candidate defect | `buffer_overflow` claim |
| Predicate | `value(copy_size) > capacity(destination)` |
| Required support | value flow from packet length to the copy-size operand |
| Required safety counterevidence | a capacity relationship, bounded range, or dominating check |
| Blocking conditions | missing producers, incomplete queries, or relevant unknown effects |

### 3.2 Build the handoff and EIR

```bash
build/bin/veritas-query evidence overflow \
  --sink memcpy --format json \
  --db "$unsafe_db" \
  > "$case_root/unsafe.slice.json"

build/bin/veritas-query evidence overflow \
  --sink memcpy --level l1 --format eir-t \
  --db "$unsafe_db" \
  > "$case_root/unsafe.l1.eir"
```

Inspect the high-value EIR members:

```bash
rg -n \
  'state =|kind = buffer_overflow|predicate = reachable|dominates_bounds_check|query_completion|path P_value_flow' \
  "$case_root/unsafe.l1.eir"
```

The generated case has this semantic shape:

```text
state = POSSIBLE_DEFECT
claim = value(copy_size) > capacity(destination)
support = MUST reachable(packet_length, memcpy_size)
check query = complete
check result = MUST_NOT dominates_bounds_check(sink, sink)
range/capacity/alias facts = absent from the current producer surface
```

`MUST_NOT` is derived only from the matching complete, scoped query and its
completion witness. It is not permission to invent the missing range or
capacity comparison. The case therefore establishes a suspicious unchecked
flow, but not a verified overflow.

### 3.3 Review decision

Disposition: **request a bound, but label the finding possible rather than
verified**.

Example review comment:

> `p->length` reaches the `memcpy` size operand, and this analysis found no
> represented dominating bounds check for the sink. The current evidence does
> not include range or destination-capacity facts, so this is a possible
> overflow rather than a proved one. Please reject or clamp lengths above
> `sizeof(b->data)` before the copy.

Record the case's `EvidenceID`, program binding, and relevant fact IDs with the
comment. Do not substitute the source line number for those semantic
identities; source anchors are presentation metadata and can move.

## 4. Use case: a visible guard that EIR cannot yet prove

### 4.1 Defect check

The safe fixture visibly guards the copy:

```cpp
void copy_payload(Packet* p, Buffer* b) {
  if (p->length <= sizeof(b->data)) {
    memcpy(b->data, p->payload, p->length);
  }
}
```

A reviewer might expect the guard to make the case `VERIFIED_SAFE`. The
current positive dominating-check producer is deferred, so that expectation
is not represented by today's Evidence input.

### 4.2 Compare the emitted case

```bash
build/bin/veritas-query evidence overflow \
  --sink memcpy --level l1 --format eir-t \
  --db "$safe_db" \
  > "$case_root/safe.l1.eir"

rg -n \
  'state =|kind = buffer_overflow|dominates_bounds_check|query_completion|path P_value_flow' \
  "$case_root/safe.l1.eir"
```

The safe and unsafe cases have different flow graphs, but both currently emit:

```text
state = POSSIBLE_DEFECT
positive dominating-check fact = absent
dominating-check query = complete-empty over the delivered relation
derived dominates_bounds_check fact = MUST_NOT
```

This result is complete over the facts the current producer materialized. It
is not a semantic proof that the source has no guard. In particular, query
completeness cannot compensate for a producer that does not yet emit positive
check facts.

### 4.3 Review decision

Disposition: **do not file a code defect from this EIR alone**. Record an
analyzer capability gap and require either manual confirmation or a future
positive check producer.

Example internal review note:

> The EIR remains `POSSIBLE_DEFECT` because the current pipeline cannot emit a
> positive dominating-check fact. The source contains a guard on every path to
> this sink, so the derived no-check member must not be used as a blocking code
> review finding. Track this as an analysis limitation.

This case demonstrates why a review renderer must show producer capabilities
alongside verification state. Hiding the deferral would turn a known analysis
limit into a false-positive code comment.

## 5. Use case: an opaque external validator

### 5.1 Defect check

The third fixture gates the sink on an external predicate whose contract is
not modeled:

```cpp
extern "C" int vendor_validate(const unsigned char* payload,
                               unsigned long length);

void copy_payload(Packet* p, Buffer* b) {
  if (vendor_validate(p->payload, p->length)) {
    memcpy(b->data, p->payload, p->length);
  }
}
```

The validator's true result does not imply any bound unless a trusted model
states that postcondition. A Review Agent must not invent one from the
function name.

### 5.2 Emit EIR and inspect the underlying unknown

```bash
build/bin/veritas-query evidence overflow \
  --sink memcpy --format json \
  --db "$opaque_db" \
  > "$case_root/opaque.slice.json"

build/bin/veritas-query evidence overflow \
  --sink memcpy --level l1 --format eir-t \
  --db "$opaque_db" \
  > "$case_root/opaque.l1.eir"
```

The current Fact Store contains an `UnknownEffect` whose reason is
`vendor_validate`. Find all current unknown facts and inspect their selected
provenance:

```bash
sqlite3 -readonly "$opaque_db/metadata.db" \
  "SELECT b.run_id, b.fact_id
     FROM run_fact_bindings AS b
     JOIN analysis_facts AS f ON f.fact_id = b.fact_id
    WHERE b.is_current = 1
      AND f.relation_name = 'UnknownEffect'
    ORDER BY b.fact_id;" \
| while IFS='|' read -r run_id fact_id
do
  build/bin/veritas-explain fact "$fact_id" \
    --run "$run_id" --db "$opaque_db" \
    --max-depth 8 --max-nodes 100 --json
done
```

One explanation contains a fact cell with `vendor_validate` and the selected
`wpa.effect.unknown.call.v2` witness. The direct semantic query API is covered
for this result, but the current public overflow CLI scopes its unknown query
such that this fact does not become an `unknown` member in the emitted case.
That projection gap is itself review-relevant: absence of an EIR `unknown`
member does not create a validator postcondition.

### 5.3 Review decision

Disposition: **keep the case possible and request a model or an explicit local
bound**.

Example review comment:

> `vendor_validate` guards this copy, but its postcondition is not modeled, so
> the analysis cannot establish that a true result bounds `p->length` to the
> destination. Please add an explicit size check at this boundary or provide a
> reviewed validator contract before treating this path as safe.

Until the unknown reaches the EIR case, attach the Fact Store explanation to
the review record and mark the EIR input as insufficient for an autonomous
decision. Do not silently enrich the case in presentation code.

## 6. Use case: a budget-truncated flow query

### 6.1 Force the bounded query to truncate

Use the unsafe database with a one-node flow budget:

```bash
build/bin/veritas-query evidence overflow \
  --sink memcpy --format json --max-nodes 1 \
  --db "$unsafe_db" \
  > "$case_root/truncated.slice.json"

build/bin/veritas-query evidence overflow \
  --sink memcpy --level l1 --format eir-t --max-nodes 1 \
  --db "$unsafe_db" \
  > "$case_root/truncated.l1.eir"
```

Inspect the M10B flow metadata without depending on `jq`:

```bash
python3 - "$case_root/truncated.slice.json" <<'PY'
import json
import sys

with open(sys.argv[1]) as stream:
    case_input = json.load(stream)

flow = case_input["flow_slice"]
checks = case_input["dominating_checks"]
print("flow nodes:", len(flow["nodes"]))
print("flow edges:", len(flow["edges"]))
print("flow metadata:", flow["metadata"])
print("check metadata:", checks["metadata"])
PY
```

The output reports zero flow nodes and edges, `completeness: truncated`, and
`truncation_reasons: [max_nodes]`. The dominating-check query is a different
query: it remains complete-empty and may still derive its scoped `MUST_NOT`
fact.

Now inspect the EIR:

```bash
rg -n \
  'state =|edge |path |unknown |omission |dominates_bounds_check|query_completion' \
  "$case_root/truncated.l1.eir"
```

The current builder emits no flow edges or path, but it also does not carry the
flow slice's truncation into an EIR `unknown` or `omission`. This is a pinned
implementation gap, not an example of correct truncation projection. The
remaining `MUST_NOT dominates_bounds_check` belongs to the separate complete
check query; it says nothing about whether the missing flow traversal was
complete.

### 6.2 Review decision

Disposition: **reject this case as insufficient input and rerun with a larger
budget**. Do not post a source-level defect or safety conclusion.

Example orchestration result:

```text
decision: NEEDS_MORE_EVIDENCE
reason: value-flow query truncated at max_nodes=1
action: rerun with a larger max_nodes budget
review_comment: none
```

A production Agent boundary should either block this EIR before dispatch or
carry the missing flow-truncation semantics after the builder is extended. It
must not infer completeness from the absence of EIR paths.

## 7. Use case: flow across a translation-unit summary

### 7.1 Defect check

The source value and sink live in different translation units:

```cpp
// entry.cpp
unsigned short packet_length(Packet* p) {
  return p->length;
}

// copy.cpp
void copy_payload(Packet* p, Buffer* b) {
  unsigned short length = packet_length(p);
  memcpy(b->data, p->payload, length);
}
```

The review question is still the same overflow predicate, but the supporting
flow must survive a function and translation-unit boundary.

### 7.2 Emit the handoff and EIR

```bash
build/bin/veritas-query evidence overflow \
  --sink memcpy --format json \
  --db "$summary_db" \
  > "$case_root/summary.slice.json"

build/bin/veritas-query evidence overflow \
  --sink memcpy --level l1 --format eir-t \
  --db "$summary_db" \
  > "$case_root/summary.l1.eir"

rg -n \
  'function_summary_id|supporting_facts|query_completion' \
  "$case_root/summary.slice.json"

rg -n \
  'state =|predicate = reachable|path P_value_flow|dominates_bounds_check' \
  "$case_root/summary.l1.eir"
```

The handoff keeps `function_summary_id` support on the flow edges, and the EIR
contains the reachable facts and value-flow path. The current case builder
does not promote that support into a top-level EIR `summary` member. Confirm
the retained summary boundary by explaining a current `GlobalFlow` fact:

```bash
sqlite3 -readonly "$summary_db/metadata.db" \
  "SELECT b.run_id, b.fact_id
     FROM run_fact_bindings AS b
     JOIN analysis_facts AS f ON f.fact_id = b.fact_id
    WHERE b.is_current = 1
      AND f.relation_name = 'GlobalFlow'
    ORDER BY b.fact_id
    LIMIT 1;" \
| while IFS='|' read -r run_id fact_id
do
  build/bin/veritas-explain fact "$fact_id" \
    --run "$run_id" --db "$summary_db" \
    --max-depth 8 --max-nodes 100 --json
done
```

The explanation contains a non-empty `summaryId` on the selected witness. That
reference is preferable to copying a callee's source or flattening its entire
summary into the review prompt.

### 7.3 Review decision

Disposition: **keep the interprocedural flow as support, preserve the summary
reference, and phrase the result as possible under current producer limits**.

Example review comment:

> The length returned by `packet_length` reaches the `memcpy` size operand
> across the translation-unit boundary. No represented capacity/range proof
> establishes that the returned value fits `b->data`. Please enforce the bound
> at the copy boundary or publish a trusted summary component that proves it.

When a reviewer needs more detail, expand the referenced summary or selected
fact witness under a new explicit budget. Do not make an unbounded source-tree
request.

## 8. Turn the cases into a review policy

The five workflows support a conservative decision procedure:

```text
if a required M10B query is truncated:
    request more evidence; do not dispatch an autonomous review comment
else if a relevant effect or validator is unknown:
    keep POSSIBLE_DEFECT and request a model or local guard
else if required producers are unavailable or complete-empty:
    keep POSSIBLE_DEFECT and name the missing proof
else if represented counterevidence discharges every admitted path:
    send the proof obligation to a deterministic verifier
else:
    report the possible defect with its EvidenceID and supporting fact IDs
```

The fourth branch is a target integration rule, not behavior delivered by the
current overflow CLI: positive checks, range/capacity facts, verifier dispatch,
and authoritative state transitions remain future work.

For every rendered review, preserve these distinctions:

| Observed state | Allowed review wording | Forbidden leap |
| --- | --- | --- |
| Flow fact with missing range/capacity | “The value reaches the sink; the bound is not established.” | “Overflow is proved.” |
| Complete-empty relation with deferred producer | “No fact is represented by this producer.” | “The source property is false.” |
| Relevant unknown effect | “The external behavior is unmodeled.” | “The validator guarantees safety.” |
| Truncated required query | “Evidence is incomplete; rerun or expand.” | “No path/fact exists.” |
| Summary-backed flow | “The selected witness crosses this summary boundary.” | Copying or inventing the callee's semantics. |

## 9. Regression anchors

The executable expectations behind these use cases live in:

- `tests/integration/evidence/OverflowEvidenceFixtureTest.cpp` for real flow,
  unknown-effect, deferred-relation, and no-fabrication behavior;
- `tests/integration/evidence/EvidenceHandoffIntegrationTest.cpp` for the
  cross-root, cross-translation-unit provenance closure;
- `tests/integration/evidence/VeritasQueryEirTest.cpp` for public CLI levels,
  representations, deterministic output, safe/unsafe distinction, and the
  pinned flow-truncation gap; and
- `tests/golden/evidence/` for reviewed semantic reference cases.

Continue with [Build an Agent-based code-review tool](tutorial-agent-code-review.md)
to place these review decisions behind a typed Agent boundary. Use the
[Evidence IR developer guide](summarydb-evidence-ir-developer-guide.md) when
adding the missing producer or projection behavior.
