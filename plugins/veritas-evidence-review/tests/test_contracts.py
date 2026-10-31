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
from veritas_review.candidates import admit_candidates
from veritas_review.contracts import admit_contract, validate_contract
from test_candidates import context, proposal
from test_review_types import binding

class ContractsTest(unittest.TestCase):
    def test_release_callback_missing_cancellation_is_inferred(self):
        c=context();p=proposal(kind="semantic_observation.v1")
        p["question"]="Queued callback may access released state after ReleaseConnection"
        state=dict(binding=binding(),contexts={c["context_id"]:c},candidates={},policy={"max_candidates":16})
        candidate=admit_candidates((p,),state).admitted[0];annotation=admit_contract(candidate,state)
        self.assertEqual(annotation["origin"],"LLM_INFERRED");self.assertEqual(annotation["status"],"PROPOSED")
        self.assertIsNone(annotation["verification_ref"])
        self.assertFalse(any("fact_id" in k for k in annotation))
        self.assertTrue(validate_contract(annotation,state))
        altered=copy.deepcopy(state);altered["contexts"][c["context_id"]]["content_digest"]="b"*64
        with self.assertRaises(ValueError):validate_contract(annotation,altered)

    def test_documented_intent_is_conditional_and_self_proof_refused(self):
        c=context();c["kind"]="DOC";p=proposal(kind="semantic_observation.v1");p["contract_refs"]=[c["context_id"]]
        state=dict(binding=binding(),contexts={c["context_id"]:c},candidates={},policy={"max_candidates":16})
        a=admit_contract(admit_candidates((p,),state).admitted[0],state)
        self.assertEqual(a["status"],"CONDITIONAL");self.assertEqual(a["origin"],"DOCUMENTED")
        a["verification_ref"]=a["candidate_id"]
        with self.assertRaises(ValueError):validate_contract(a,state)
