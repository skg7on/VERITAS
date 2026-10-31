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


"""Deterministic admission before EIR; unresolved proposals retain coverage."""
from dataclasses import dataclass
import copy
from . import review_types as t
from .context import validate_anchor

@dataclass(frozen=True)
class Admission:
    admitted: tuple
    rejected: tuple
    deferred: tuple


def admit_candidates(proposals, state):
    groups={};rejected=[];deferred=[]
    for proposal in proposals:
        try:
            p=t._wire(proposal) if isinstance(proposal,t.CandidateProposal) else copy.deepcopy(proposal)
            t.CandidateProposal.from_wire(p)
            if p["binding"]!=state["binding"]:raise ValueError("stale snapshot")
            if not p["anchor_refs"]:raise ValueError("candidate needs an advertised anchor")
            calls=[];anchors=[];ambiguous=False
            for anchor in p["anchor_refs"]:
                matches=validate_anchor(anchor,state)
                sites=[e["entity_ref"] for e in matches if e["kind"]=="CALLSITE"]
                ambiguous=ambiguous or len(sites)>1
                normalized=copy.deepcopy(anchor)
                if len(sites)==1:normalized["entity_ref"]=sites[0]
                anchors.append(normalized);calls.extend(sites)
            for ref in p["contract_refs"]:
                if ref not in state["contexts"]:raise ValueError("unadvertised contract context")
            anchors=sorted({t.canonical_json(a):a for a in anchors}.values(),key=t.canonical_json)
            identity=dict(binding=p["binding"],claim_kind=p["claim_kind"],anchor_refs=anchors,contract_refs=sorted(set(p["contract_refs"])))
            semantic_anchors=sorted({t.canonical_json(a["entity_ref"] if a["entity_ref"] is not None else a) for a in anchors})
            cid=t.artifact_id(t.CandidateId,{**identity,"anchor_refs":[raw.decode("utf-8") for raw in semantic_anchors]}).value
            # Exact span resolution, never choose from proximity or a model ordinal.
            unique_calls={t.canonical_json(ref):ref for ref in calls}
            target=next(iter(unique_calls.values())) if len(unique_calls)==1 and not ambiguous else None
            candidate={**identity,"candidate_id":cid,"origins":[p["origin"]],"questions":[p["question"]],
                       "assumptions":sorted(set(p["assumptions"])),"target":target if p["claim_kind"]=="copy_bound.v1" else None}
            if p["claim_kind"]=="copy_bound.v1" and target is None:
                deferred.append(dict(proposal=p,reason="unresolved or ambiguous exact callsite"));continue
            if cid in groups:
                old=groups[cid]
                for key in ("origins","questions","assumptions"):old[key]=sorted(set(old[key]+candidate[key]))
                old["anchor_refs"]=sorted({t.canonical_json(a):a for a in old["anchor_refs"]+candidate["anchor_refs"]}.values(),key=t.canonical_json)
            else:groups[cid]=candidate
        except ValueError as e:
            rejected.append(dict(proposal=proposal if type(proposal) is dict else t._wire(proposal),reason=str(e)))
    admitted=[];remaining=state["policy"]["max_candidates"]-len(state["candidates"])
    for cid,c in sorted(groups.items()):
        if cid in state["candidates"]:
            previous=state["candidates"][cid]
            for key in ("origins","questions","assumptions"):c[key]=sorted(set(previous[key]+c[key]))
            c["anchor_refs"]=sorted({t.canonical_json(a):a for a in previous["anchor_refs"]+c["anchor_refs"]}.values(),key=t.canonical_json)
        elif remaining>0:remaining-=1
        else:
            deferred.append(dict(candidate=c,reason="candidate limit"));continue
        admitted.append(c)
    return Admission(tuple(admitted),tuple(sorted(rejected,key=t.canonical_json)),tuple(sorted(deferred,key=t.canonical_json)))
