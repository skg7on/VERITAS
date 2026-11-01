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

"""Fail-closed administrative qualification; this never grants EIR authority.

Independent judgments are trusted operator inputs bound to response bytes.
Model responses and their self-reported outcomes cannot adjudicate themselves.
"""
from __future__ import annotations
from collections import Counter,defaultdict
import hashlib
import json
import math
from pathlib import Path
import statistics

try:
    from .run import digest
except ImportError:
    from run import digest

OUTCOMES={"TRUE_POSITIVE_SUPPORTED","TRUE_POSITIVE_UNPROVEN","FALSE_POSITIVE","TRUE_NEGATIVE","FALSE_NEGATIVE","ABSTENTION","UNSUPPORTED"}
PHASES=("cold_setup_ms","cold_index_ms","provider_bootstrap_ms","model_ms","warm_query_ms")
MEASUREMENTS=PHASES+("end_to_end_ms","aggregate_peak_rss_bytes","physical_footprint_bytes","live_store_bytes","physical_store_bytes","model_turns","input_tokens","output_tokens","model_cost_usd","reviewer_ms","load_average","swap_bytes")


def number(value):
    return type(value) in (float,int) and math.isfinite(value) and value>=0


def distribution(values):
    values=sorted(values)
    return {"samples":len(values),"min":min(values) if values else None,
            "median":statistics.median(values) if values else None,"max":max(values) if values else None,
            "p95":values[math.ceil(.95*len(values))-1] if len(values)>=100 else None}


def measurement_gaps(data):
    gaps=[key for key in MEASUREMENTS if not number(data.get(key))]
    if not gaps and data["end_to_end_ms"]+0.01 < sum(data[key] for key in PHASES):
        gaps.append("end_to_end_ms below phase total")
    return gaps


def quality(rows):
    result={}
    for mode in ("fixed_candidate","whole_discovery"):
        selected=[r for r in rows if r["mode"]==mode];counts=Counter(r["outcome"] for r in selected)
        tp=counts["TRUE_POSITIVE_SUPPORTED"];fp=counts["FALSE_POSITIVE"];fn=counts["FALSE_NEGATIVE"]
        positive_rows=[r for r in selected if r.get("positive",True)]
        supported=sum(r["outcome"]=="TRUE_POSITIVE_SUPPORTED" for r in positive_rows)
        result[mode]={"attempts":len(selected),"counts":dict(sorted(counts.items())),
                      "precision":tp/(tp+fp) if tp+fp else None,
                      "recall":supported/len(positive_rows) if positive_rows else None,
                      "misses":sorted({r["case_id"] for r in selected if r.get("positive",True) and r["outcome"]!="TRUE_POSITIVE_SUPPORTED"}),
                      "supported_reports":tp,"deciding":sum(bool(r.get("analytically_deciding",False)) for r in selected if r["outcome"]=="TRUE_POSITIVE_SUPPORTED"),"useful_hypotheses":counts["TRUE_POSITIVE_UNPROVEN"],
                      "abstention_rate":counts["ABSTENTION"]/len(selected) if selected else None,
                      "unsupported_rate":counts["UNSUPPORTED"]/len(selected) if selected else None}
    return result


def adjudicate(attempt, judgment, model):
    errors=[]
    if judgment.get("response_digest")!=attempt.get("response_digest"):errors.append("adjudication response mismatch")
    if not judgment.get("reviewer") or judgment["reviewer"]==model:errors.append("independent adjudicator required")
    if judgment.get("outcome") not in OUTCOMES:errors.append("invalid adjudication outcome")
    if judgment.get("false_promotion") is not False:errors.append("zero false promotion not evidenced")
    return judgment.get("outcome"),errors


def _artifacts(attempt):
    """Resolve only the recorded response, check immutable bytes before scoring."""
    directory=Path(attempt["artifact_directory"])
    raw=(directory/"response.json").read_bytes()
    if "sha256:"+hashlib.sha256(raw).hexdigest()!=attempt["response_digest"]:raise ValueError("response bytes changed")
    request=json.loads((directory/"request.json").read_text())
    if digest(request)!=attempt["request_digest"]:raise ValueError("request bytes changed")
    return request,json.loads(raw)


def evaluate(corpus,bundle,judgments):
    gaps=[];rows=[];workloads=defaultdict(list);seen=set();identities=set();configs=set();artifact_dirs=set();request_digests=set()
    if bundle.get("version")!="veritas.review.eval.run.v1":gaps.append("invalid run version")
    if bundle.get("release_digest")!=corpus["release_digest"]:gaps.append("corpus release mismatch")
    config=bundle.get("configuration") or {}
    if not config:gaps.append("operator model/disclosure configuration pending")
    judge_map={}
    for judgment in judgments:
        key=judgment.get("attempt_id")
        if key in judge_map:gaps.append("duplicate adjudication")
        judge_map[key]=judgment
    cases={c["case_id"]:c for f in corpus["families"] for c in f["cases"]}
    attempts=bundle.get("attempts",[])
    if not attempts:gaps.append("no attempts")
    for attempt in attempts:
        aid=attempt.get("attempt_id")
        if not isinstance(aid,str) or aid in seen:
            gaps.append("duplicate or invalid attempt identity");continue
        seen.add(aid)
        try:
            p,m,c=attempt["profile"],attempt["mode"],attempt["case_id"]
            run_id,sample_id=attempt["run_id"],attempt["sample_id"]
            identity=(p,m,c,run_id,sample_id)
            if identity in identities:raise ValueError("duplicate workload sample")
            identities.add(identity)
            if type(run_id) is not int or run_id<0 or type(sample_id) is not int or sample_id<0:raise ValueError("invalid sample identity")
            if c not in corpus["runs"][m]["case_ids"]:raise ValueError("case outside frozen run")
            budget=corpus["budgets"]["profiles"][p]
            if budget["availability"]!="AVAILABLE":raise ValueError("unavailable profile was executed")
            if attempt["configuration_digest"]!=digest(config):raise ValueError("mixed model/policy configuration")
            configs.add(attempt["configuration_digest"])
            if attempt["status"]!="EXECUTED":raise ValueError("attempt did not execute successfully")
            job={key:attempt[key] for key in ("profile","mode","case_id","run_id","sample_id")}
            directory=str(Path(attempt["artifact_directory"]).resolve())
            if directory in artifact_dirs or attempt["request_digest"] in request_digests:raise ValueError("reused attempt artifacts/request")
            request,response=_artifacts(attempt)
            if request.get("attempt_nonce")!=digest(job):raise ValueError("request nonce does not match sample")
            artifact_dirs.add(directory);request_digests.add(attempt["request_digest"])
            if request["profile"]!=p or request["mode"]!=m or request["budget"]!=budget:raise ValueError("request pairing/budget mismatch")
            if request["configuration"]!=config:raise ValueError("request configuration mismatch")
            case=cases[c]
            if request["base_revision"]!=case["base_revision"] or request["head_revision"]!=case["head_revision"]:raise ValueError("revision mismatch")
            if response.get("origin")!="PRODUCTION_CONTROLLER":raise ValueError("practice/fake response cannot qualify")
            metrics=dict(response["measurements"])
            metrics["aggregate_peak_rss_bytes"]=attempt.get("aggregate_peak_rss_bytes")
            missing=measurement_gaps(metrics)
            if missing:raise ValueError("missing/invalid measurements: "+", ".join(missing))
            if any(metrics[key]>budget[key] for key in ("model_turns","input_tokens","output_tokens") if key in budget):raise ValueError("frozen budget exceeded")
            if metrics["model_turns"]>budget["max_turns"] or attempt["runner_wall_ms"]>budget["wall_ms"]:raise ValueError("frozen budget exceeded")
            if not number(attempt.get("aggregate_peak_rss_bytes")):raise ValueError("aggregate process RSS unavailable")
            judgment=judge_map.get(aid)
            if not judgment:raise ValueError("independent adjudication pending")
            outcome,errors=adjudicate(attempt,judgment,config.get("model"))
            if errors:raise ValueError(", ".join(errors))
            # Independent operator checks the native authority receipt, exact site,
            # controls and source anchors; the model's status is never that check.
            if not judgment.get("evidence_refs") or judgment.get("evidence_accurate") is not True:raise ValueError("independent evidence check missing")
            for ref in judgment["evidence_refs"]:
                raw=Path(ref["path"]).read_bytes()
                if "sha256:"+hashlib.sha256(raw).hexdigest()!=ref["digest"]:raise ValueError("adjudication witness digest mismatch")
            positive=case["oracle"]["kind"]!="CLEAN"
            if positive and outcome=="TRUE_NEGATIVE":raise ValueError("known positive mislabeled negative")
            if not positive and outcome!="TRUE_NEGATIVE":gaps.append("negative control failed: "+aid)
            if positive and outcome!="TRUE_POSITIVE_SUPPORTED":gaps.append("missed or unsupported positive: "+aid)
            if outcome=="TRUE_POSITIVE_SUPPORTED":
                if case["family"]=="local-bounds":
                    required={"exact_site_refuted","qualified_isolation","base_rejection","boundary_n","short_source","wrong_site","unrelated_crash"}
                    if not required<=set(judgment.get("checks",[])):raise ValueError("real deciding replay/controls missing")
                else:
                    if judgment.get("semantic_status") not in ("PROPOSED","CONDITIONAL"):raise ValueError("unsupported semantic promotion")
                    if not {"enqueue","cancel","dispatch","release"}<=set(judgment.get("checks",[])):raise ValueError("semantic anchors missing")
            rows.append(dict(attempt,positive=positive,outcome=outcome,measurements=metrics,analytically_deciding=positive and case["family"]=="local-bounds" and outcome=="TRUE_POSITIVE_SUPPORTED"))
            workloads[(p,m,c,run_id)].append(rows[-1])
        except (KeyError,ValueError,TypeError,OSError) as error:
            gaps.append(f"{aid}: {error}")
    required_profiles=[p for p,v in corpus["budgets"]["profiles"].items() if v["availability"]=="AVAILABLE"]
    for p in required_profiles:
        for m,policy in corpus["runs"].items():
            for c in policy["case_ids"]:
                runs={key[3] for key in workloads if key[:3]==(p,m,c)}
                if len(runs)<3:gaps.append(f"{p}/{m}/{c}: fewer than 3 fresh full runs")
                for run_id in runs:
                    samples=workloads[(p,m,c,run_id)]
                    if len(samples)<100:gaps.append(f"{p}/{m}/{c}/{run_id}: fewer than 100 attempts; P95 unavailable")
    # Every profile must have the same admitted run/sample grid for each case/mode.
    grids={p:{(a["mode"],a["case_id"],a["run_id"],a["sample_id"]) for a in rows if a["profile"]==p} for p in required_profiles}
    if any(grid!=grids[required_profiles[0]] for grid in grids.values()):gaps.append("unpaired profile workloads")
    profiles={}
    for p,policy in corpus["budgets"]["profiles"].items():
        if policy["availability"]!="AVAILABLE":profiles[p]=dict(policy);continue
        selected=[a for a in rows if a["profile"]==p]
        profiles[p]={"availability":"AVAILABLE","precision":None,"quality":quality(selected),
                     "workloads":{f"{m}/{c}/run-{n}":{metric:distribution([a["measurements"][metric] for a in attempts]) for metric in MEASUREMENTS}
                                  for (profile,m,c,n),attempts in sorted(workloads.items()) if profile==p},
                     "run_variability":{f"{m}/run-{n}":quality([a for a in selected if a["run_id"]==n])[m]
                                        for m in corpus["runs"] for n in sorted({a["run_id"] for a in selected})}}
    evaluation_status="INCOMPLETE" if gaps else "COMPLETE"
    gaps.append("production release validator unavailable")
    return {"version":"veritas.review.eval.decision.v1","release_digest":corpus["release_digest"],
            "qualification":"OPEN","evaluation_status":evaluation_status,"p4_investment":"OPEN: requires held-out incremental quality/cost decision",
            "configuration":config,"profiles":profiles,"admitted_attempts":len(rows),"gaps":sorted(set(gaps)),
            "authority":"administrative evaluation only; no native facts or universal-safety promotion",
            "scaling":"#142/#145 thresholds unchanged; no demand-scaling claim"}
