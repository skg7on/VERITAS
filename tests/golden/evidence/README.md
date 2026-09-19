# Evidence IR goldens

Copyright 2026 VERITAS Contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

## Why the `.eir` files here carry no license header

The `.eir` files beside this notice are **canonical EIR-T**, and canonical form
removes comments (formal specification §19.1, clause 4: "Comments are removed in
canonical form") — so a header comment would either be stripped by the writer,
leaving the file non-canonical and breaking the REP-001 write/parse/write byte
fixpoint, or survive into the content-addressed bytes and change every
`EvidenceID` derived from them.

## Contents

| File | What it pins |
| --- | --- |
| `overflow_unsafe.l0.eir` | The unsafe fixture projected at `l0`, with the member-withholding omissions a level projection is required to name. |
| `overflow_unsafe.l1.eir` | The same case at `l1`: the level M10B's demo target actually emits. |
| `overflow_unsafe.l1.eir.json` | The full-fidelity JSON rendering of `overflow_unsafe.l1.eir`. **Read** by DEM-005 and required to be byte-identical to `ToEvidenceJson` of the case the `.eir` beside it parses into, so the two halves of one case cannot drift apart. (M10C ships no JSON *reader*, so this golden is never parsed back into a case; the comparison runs from the text side.) |
| `overflow_safe.l1.eir` | The safe fixture at `l1`. The difference from unsafe is flow shape, not verdict — both are `POSSIBLE_DEFECT` and both carry the derived `MUST_NOT dominates_bounds_check`. |
| `overflow_truncated.l1.eir` | The unsafe fixture analysed with `--max-nodes 1`. |
| `overflow_unsafe.slice.json` | **Not an EIR artefact.** The M10B `EvidenceInput` slice, delivered by M10B. Do not regenerate. |

## How they are used

`tests/integration/evidence/VeritasQueryEirTest.cpp` compares the CLI's output
against these files. The `.eir` files are a *reference for meaning*, not a byte
oracle across machines: content addresses embed LLVM's host-dependent
`target-features` attribute and `analysis_run_id` hashes the vendored Soufflé
executable, so two machines legitimately disagree on the bytes. The comparison
is therefore semantic. Byte equality is asserted only where both sides come from
the same build — see that file's header comment for the split and the reasons.
