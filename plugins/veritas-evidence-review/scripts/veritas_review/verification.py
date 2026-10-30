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


"""Exact-obligation authority boundary. No production verifier is registered."""
from dataclasses import dataclass
from types import MappingProxyType
from . import review_types as t

@dataclass(frozen=True)
class VerificationPolicy:
    producers: frozenset
    artifacts: object
    authorized_results: frozenset
    admitted_assumptions: frozenset

    def __post_init__(self):
        object.__setattr__(self, "artifacts", MappingProxyType(dict(self.artifacts)))
        for key in ("producers", "authorized_results", "admitted_assumptions"):
            object.__setattr__(self, key, frozenset(getattr(self, key)))

def obligation_id(request):
    return t.artifact_id(t.ObligationId,{k:v for k,v in request.items() if k != "obligation_id"}).value

def request_digest(request):
    return t.hash_bytes(t.canonical_json(request))

def verification_id(result):
    return t.artifact_id(t.VerificationId,{k:v for k,v in result.items() if k != "verification_id"}).value

def validate_verification_request(request):
    t.VerificationRequest.from_wire(request)
    domain=request["execution_domain"]; operands=request["typed_operands"]
    if (request["obligation_id"] != obligation_id(request) or
        domain["snapshot_id"] != request["binding"]["snapshot_id"] or
        domain["input_digest"] != operands["payload_digest"] or
        domain["harness_id"] != operands["harness_id"] or
        operands["requested_bytes"] > operands["readable_bytes"]):
        raise ValueError("invalid obligation or concrete input domain")

def validate_verification_result(request, result, policy):
    validate_verification_request(request)
    t.VerificationResult.from_wire(result)
    if result["verification_id"] != verification_id(result): raise ValueError("result identity mismatch")
    for key in ("binding", "predicate_kind", "typed_operands", "execution_domain"):
        if request[key] != result[key]: raise ValueError("verification request mismatch")
    if result["request_digest"] != request_digest(request): raise ValueError("request digest mismatch")
    producer=result["producer"]
    if (producer["name"],producer["version"],producer["config_digest"]) not in policy.producers:
        raise ValueError("unregistered producer")
    if (producer["name"],producer["version"]) != (request["backend_id"],request["backend_version"]):
        raise ValueError("wrong requested producer")
    if result["assumptions"] != request["assumptions"]: raise ValueError("assumption mismatch")
    for assumption in result["assumptions"]:
        if (assumption not in policy.admitted_assumptions or
            request["obligation_id"] in assumption or result["verification_id"] in assumption):
            raise ValueError("unadmitted or circular assumption")
    deps={}
    for dependency in result["input_dependencies"]:
        key=dependency["artifact_id"]
        if key in deps or key in (request["obligation_id"],result["verification_id"]):
            raise ValueError("duplicate or circular dependency")
        if key not in policy.artifacts or t.hash_bytes(policy.artifacts[key]) != dependency["content_digest"]:
            raise ValueError("dependency content mismatch")
        deps[key]=dependency["content_digest"]
    refs=request["input_artifact_ids"]+result["certificate_artifact_ids"]+result["counterexample_artifact_ids"]
    if any(ref not in deps for ref in refs): raise ValueError("missing dependency")
    deciding=result["status"] in ("PROVED","REFUTED")
    if deciding:
        coverage=result["coverage"]
        witness=result["certificate_artifact_ids"] if result["status"]=="PROVED" else result["counterexample_artifact_ids"]
        if (result["operational_outcome"] != "COMPLETED" or not witness or
            not coverage["supported"] or coverage["traversal"] != "COMPLETE" or coverage["frontier_refs"] or
            result["verification_id"] not in policy.authorized_results):
            raise ValueError("no independent exact-request authority")
        if set(witness) & set(request["input_artifact_ids"]): raise ValueError("input reused as its own proof")
        operands=request["typed_operands"]
        holds=operands["requested_bytes"] <= operands["capacity_bytes"]
        if (result["status"]=="PROVED") != holds: raise ValueError("wrong bound polarity")

def apply_verification(state, request, result, policy):
    if state not in {"INCONCLUSIVE","SUPPORTED_CONCERN","SEMANTIC_OBSERVATION","VERIFIED_FINDING","NO_DEFECT_OBSERVED"}:
        raise ValueError("unknown disposition")
    validate_verification_result(request,result,policy)
    if result["status"]=="REFUTED": return "VERIFIED_FINDING"
    if result["status"]=="PROVED": return "NO_DEFECT_OBSERVED"
    return state
