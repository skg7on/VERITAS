# Copyright 2026 VERITAS Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


"""P1 annotations retain inferred intent without producing program facts."""
from . import review_types as t


def admit_contract(candidate, state):
    if candidate["claim_kind"]!="semantic_observation.v1":raise ValueError("semantic annotation required")
    refs=sorted({a["context_id"] for a in candidate["anchor_refs"]}|set(candidate["contract_refs"]))
    if any(ref not in state["contexts"] for ref in refs):raise ValueError("unadvertised contract reference")
    has_doc=any(state["contexts"][ref]["kind"]=="DOC" for ref in candidate["contract_refs"])
    # Documentation cites intent; it cannot validate implementation or a schedule.
    return dict(candidate_id=candidate["candidate_id"],binding=state["binding"],proposal_digest=t.hash_bytes(t.canonical_json(candidate)),subject_refs=candidate["anchor_refs"],
        predicate=dict(kind="semantic_observation.v1",text=candidate["questions"]),
        preconditions=[],effects=[],invariants=[],evidence_refs=refs,
        origin="DOCUMENTED" if has_doc else "LLM_INFERRED",assumptions=candidate["assumptions"],
        version="semantic-contract.v1",dependencies=[dict(context_id=ref,content_digest=state["contexts"][ref]["content_digest"]) for ref in refs],
        status="CONDITIONAL" if has_doc or candidate["assumptions"] else "PROPOSED",verification_ref=None)


def validate_contract(annotation, state):
    """Reuse is observation-only and invalidates on source, model or doc changes."""
    if annotation["binding"]!=state["binding"] or annotation["status"] not in {"PROPOSED","CONDITIONAL"} or annotation["verification_ref"] is not None:
        raise ValueError("P1 annotation has no independent proof authority")
    for dep in annotation["dependencies"]:
        artifact=state["contexts"].get(dep["context_id"])
        if artifact is None or artifact["content_digest"]!=dep["content_digest"]:raise ValueError("stale contract dependency")
    current=state.get("candidates",{}).get(annotation["candidate_id"])
    if current is not None and t.hash_bytes(t.canonical_json(current))!=annotation["proposal_digest"]:raise ValueError("stale model proposal")
    return True
