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


"""Deterministic semantic reports; audit occurrence/costs remain separate."""
import copy
import html
import re
from . import review_types as t
from .audit import AuditStore
from .contracts import admit_contract


def render_report(state):
    if not isinstance(state,dict):
        store=AuditStore(state.state_dir)
        with store.locked():data=store.load()
    else:data=state
    candidates=[];annotations=[]
    for cid,c in sorted(data["candidates"].items()):
        assessment=data["assessments"].get(cid)
        evidence=[e for e in data["evidence"].values() if e["callsite_ref"]==c["target"]]
        category="SEMANTIC OBSERVATION" if c["claim_kind"]=="semantic_observation.v1" else "INCONCLUSIVE"
        if evidence and assessment and assessment["assessment"]=="likely_defect" and any(e["observations"] for e in evidence):category="SUPPORTED CONCERN"
        candidates.append({**copy.deepcopy(c),"category":category,"assessment":assessment,
            "supporting_refs":sorted(set([e["local_evidence_id"] for e in evidence]+(assessment["refs"] if assessment and assessment["assessment"]=="likely_defect" else []))),"contradicting_refs":sorted(set(assessment["refs"])) if assessment and assessment["assessment"]=="likely_false_positive" else [],"verification_refs":[]})
        if c["claim_kind"]=="semantic_observation.v1":annotations.append(admit_contract(c,data))
    coverage=copy.deepcopy(data["coverage"])
    missing=set(data["context_refs"])-set(data["contexts"])
    for ref in sorted(missing):
        coverage["deferred"].append(dict(context_id=ref,reason="Advertised context was not visited before "+data["status"]))
    report=dict(schema="review-report.v1",binding=data["binding"],status=data["status"],mode="production",
        capability_profile="local-observation-only",coverage=coverage,candidates=candidates,
        annotations=annotations,unknowns=sorted(set(data["unknowns"]+["No safety conclusion; empty or incomplete discovery cannot establish absence.",
            "Production verification and native evidence are unavailable.","Live tree advancement has not been checked; all citations use the captured snapshot."])),
        assumptions=sorted({a for c in candidates for a in c["assumptions"]}),
        local_evidence=sorted(data["evidence"].values(),key=lambda e:e["local_evidence_id"]))
    # Real usage/timing and audit hashes are deliberately excluded from semantic ID.
    semantic=copy.deepcopy(report)
    for evidence in semantic["local_evidence"]:evidence.pop("usage",None)
    report["report_id"]=t.artifact_id(t.ReviewReportId,semantic).value
    report["costs"]=copy.deepcopy(data["costs"])
    if len(t.canonical_json(report))>data["policy"]["report_bytes"]:raise ValueError("report byte budget")
    return report


def semantic_report_json(report):
    value=copy.deepcopy(report);value.pop("costs",None)
    for e in value["local_evidence"]:e.pop("usage",None)
    return t.canonical_json(value)


def _escape(text):
    return re.sub(r"([\\`*_{}\[\]()#+.!|>~-])",r"\\\1",html.escape(text,quote=True)).replace("\n"," ")


def render_markdown(report):
    lines=["VERITAS review — "+report["status"],"", "No safety conclusion. Evidence is scoped to the captured snapshot.",""]
    for c in report["candidates"]:
        lines.append(c["category"]+": "+_escape("; ".join(c["questions"])))
        for a in c["anchor_refs"]:lines.append("  Citation: "+_escape(a["context_id"])+" bytes "+str(a["start"])+"–"+str(a["end"]))
    lines.extend(["", "Unknowns:"]+[_escape(s) for s in report["unknowns"]])
    return "\n".join(lines)+"\n"
