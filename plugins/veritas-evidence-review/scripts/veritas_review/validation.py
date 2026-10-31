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


"""Closed, bounded model boundary; source strings never select operations."""
import json
from . import review_types as t


def closed(value, keys):
    if type(value) is not dict or set(value)!=set(keys):raise ValueError("unknown or missing key")


def decode_response(value, policy):
    if type(value) is dict:value=t.canonical_json(value)
    if type(value) is not bytes or len(value)>policy.response_bytes:raise ValueError("model response byte limit")
    def pairs(entries):
        result={}
        for k,v in entries:
            if k in result:raise ValueError("duplicate JSON key")
            result[k]=v
        return result
    try:
        result=json.loads(value.decode("utf-8","strict"),object_pairs_hook=pairs)
        t.canonical_json(result)
    except (UnicodeError, RecursionError) as e:raise ValueError("invalid model JSON") from e
    closed(result,("binding","candidate_proposals","assessments","requests","finish"))
    t.ReviewBinding.from_wire(result["binding"])
    if type(result["finish"]) is not bool:raise ValueError("boolean finish required")
    for key,limit in (("candidate_proposals",16),("assessments",16),("requests",16)):
        if type(result[key]) is not list or len(result[key])>limit:raise ValueError("response item limit")
    for p in result["candidate_proposals"]:
        t.CandidateProposal.from_wire(p)
        if p["origin"]!="LLM":raise ValueError("model proposals must have LLM origin")
    for a in result["assessments"]:
        closed(a,("candidate_id","assessment","refs","assumptions"))
        t.CandidateId(a["candidate_id"])
        if type(a["assessment"]) is not str or a["assessment"] not in {"needs_evidence","likely_defect","likely_false_positive","inconclusive"}:raise ValueError("model authority rejected")
        if type(a["refs"]) is not list or any(type(r) is not str for r in a["refs"]):raise ValueError("reference list required")
        if type(a["assumptions"]) is not list or any(type(s) is not str for s in a["assumptions"]):raise ValueError("assumptions required")
    ids=set()
    for request in result["requests"]:
        closed(request,("request_id","operation","parameters"))
        if type(request["request_id"]) is not str or not request["request_id"] or request["request_id"] in ids:raise ValueError("duplicate or invalid request ID")
        ids.add(request["request_id"])
    return result
