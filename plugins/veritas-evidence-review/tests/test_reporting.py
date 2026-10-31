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
import test_review_controller
from veritas_review.reporting import render_report, semantic_report_json

class ReportingTest(unittest.TestCase):
    setUp=test_review_controller.ControllerTest.setUp
    response=test_review_controller.ControllerTest.response
    advance=test_review_controller.ControllerTest.advance

    def test_false_positive_citations_are_contradicting(self):
        candidate=self.turn.as_dict()["candidates"][0]
        ref=self.turn.contexts[0]["context_id"]
        r=self.response();r["assessments"]=[dict(candidate_id=candidate["candidate_id"],assessment="likely_false_positive",refs=[ref],assumptions=["Guard covers the selected execution"])]
        self.advance(r)
        c=render_report(self.state)["candidates"][0]
        self.assertEqual(c["contradicting_refs"],[ref]);self.assertNotEqual(c["category"],"VERIFIED FINDING")

    def test_local_usage_does_not_change_semantic_report(self):
        r=self.response();r["requests"]=[next(g for g in self.turn.requests if g["operation"]=="local-evidence")]
        self.advance(r)
        left=render_report(self.state);right=copy.deepcopy(left)
        right["costs"]["backend_wall_ms"]+=100
        right["local_evidence"][0]["usage"]["wall_ms"]+=100
        self.assertEqual(semantic_report_json(left),semantic_report_json(right))
