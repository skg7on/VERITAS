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



import copy
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"scripts"))
from veritas_review import review_types as t
from veritas_review import verification as v


def fixture():
    aid=lambda cls: t.artifact_id(cls,{}).value
    digest="a"*64
    binding={"snapshot_id":aid(t.SnapshotId),"repository_id":"repo:sha256:"+digest,
             "base_revision_id":"git:"+"b"*40,"working_tree_digest":digest,
             "program_scope_id":aid(t.ProgramScopeId),"build_context_digest":digest,
             "native_binding":None,"provider_versions":[]}
    entity={"index_id":aid(t.StructuralIndexId),"ordinal":0}
    request={"binding":binding,"predicate_kind":"copy_bound.v1",
       "typed_operands":{"kind":"copy_bound.v1","callsite_ref":entity,
          "destination_ref":entity,"source_ref":entity,"length_ref":entity,
          "capacity_bytes":32,"requested_bytes":33,"readable_bytes":33,
          "payload_digest":t.hash_bytes(b"payload"),"harness_id":"test-harness"},
       "input_artifact_ids":[aid(t.LocalEvidenceId)],"assumptions":[],
       "execution_domain":{"kind":"CONCRETE_INPUT","snapshot_id":binding["snapshot_id"],
          "input_digest":t.hash_bytes(b"payload"),"harness_id":"test-harness"},
       "backend_id":"test-bound-check","backend_version":"1",
       "budget":{"wall_ms":1000,"output_bytes":4096,"examined":100}}
    request["obligation_id"]=v.obligation_id(request)
    result={"binding":binding,"request_digest":v.request_digest(request),
       "predicate_kind":"copy_bound.v1","typed_operands":copy.deepcopy(request["typed_operands"]),
       "execution_domain":copy.deepcopy(request["execution_domain"]),
       "producer":{"name":"test-bound-check","version":"1","config_digest":digest},
       "status":"REFUTED","proposition":"REQUESTED_LE_CAPACITY",
       "certificate_artifact_ids":[],"counterexample_artifact_ids":["witness"],
       "input_dependencies":[{"artifact_id":aid(t.LocalEvidenceId),"content_digest":t.hash_bytes(b"evidence")},
                              {"artifact_id":"witness","content_digest":t.hash_bytes(b"witness")}],
       "assumptions":[],"coverage":{"supported":True,"scope_closed":False,
          "traversal":"COMPLETE","frontier_refs":[],"reasons":[],"examined":1},
       "operational_outcome":"COMPLETED"}
    result["verification_id"]=v.verification_id(result)
    policy=v.VerificationPolicy(producers=frozenset({("test-bound-check","1",digest)}),
       artifacts={aid(t.LocalEvidenceId):b"evidence","witness":b"witness"},
       authorized_results=frozenset({result["verification_id"]}),admitted_assumptions=frozenset())
    return request,result,policy

class VerificationTest(unittest.TestCase):
    def test_ExactRegisteredCertificateAloneMayDecide(self):
        q,r,p=fixture()
        self.assertEqual(v.apply_verification("SUPPORTED_CONCERN",q,r,p),"VERIFIED_FINDING")
        self.assertEqual(v.apply_verification("SUPPORTED_CONCERN",q,r,p),"VERIFIED_FINDING")

    def test_WrongBindingPredicateOperandProducerInputAndCircularProof(self):
        q,r,p=fixture()
        mutations=[lambda x:x["binding"].update(working_tree_digest="c"*64),
                   lambda x:x.update(predicate_kind="invented"),
                   lambda x:x["typed_operands"].update(capacity_bytes=64),
                   lambda x:x["producer"].update(name="model"),
                   lambda x:x["input_dependencies"][0].update(content_digest="c"*64),
                   lambda x:x["assumptions"].append(q["obligation_id"])]
        for mutation in mutations:
            changed=copy.deepcopy(r); mutation(changed)
            changed["verification_id"]=v.verification_id(changed)
            modified=v.VerificationPolicy(p.producers,p.artifacts,frozenset({changed["verification_id"]}),p.admitted_assumptions)
            with self.subTest(mutation=mutations.index(mutation)), self.assertRaises(ValueError):
                v.apply_verification("SUPPORTED_CONCERN",q,changed,modified)

    def test_CopiedOrUnregisteredProofCannotPromote(self):
        q,r,p=fixture()
        p=v.VerificationPolicy(p.producers,p.artifacts,frozenset(),p.admitted_assumptions)
        with self.assertRaises(ValueError):v.validate_verification_result(q,r,p)

    def test_TimeoutUnsupportedAndIncompleteCannotPromote(self):
        q,r,p=fixture()
        for status,outcome in [("UNKNOWN","TIMEOUT"),("INCOMPLETE","UNSUPPORTED"),("ERROR","ERROR")]:
            x=copy.deepcopy(r); x.update(status=status,operational_outcome=outcome)
            x["verification_id"]=v.verification_id(x)
            self.assertEqual(v.apply_verification("SUPPORTED_CONCERN",q,x,p),"SUPPORTED_CONCERN")
        r["operational_outcome"]="TIMEOUT"
        with self.assertRaises(ValueError):v.validate_verification_result(q,r,p)

    def test_UndefinedOrUniversalDomainIsRejected(self):
        q,r,p=fixture();q["execution_domain"]={"kind":"FORMAL","domain":[]}
        with self.assertRaises(ValueError):v.validate_verification_result(q,r,p)

    def test_VerifyWireIsClosedAndIdentityExcludesAudit(self):
        q,r,p=fixture()
        data={"protocol":t.PROTOCOL,"request_id":"verify-1","operation":"verify","parameters":q}
        self.assertEqual(t.decode_v2(t.canonical_json(data)).operation,"verify")
        data["parameters"]["command"]="echo fake"
        with self.assertRaises(ValueError):t.decode_v2(t.canonical_json(data))
