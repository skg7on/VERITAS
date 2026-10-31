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


import json
import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"scripts"))
import test_native_backend
from veritas_review.controller import start_review,advance_review
from veritas_review.models import ReviewOptions,ReviewPolicy
from veritas_review.reporting import render_report,semantic_report_json

class ControllerNativeTest(unittest.TestCase):
    setUp=test_native_backend.NativeBackendTest.setUp
    git=test_native_backend.NativeBackendTest.git
    def test_real_start_advance_source_annotation_local_report(self):
        options=ReviewOptions(state_dir=Path(self.temp.name)/"state",project_key="fixture",base_ref="HEAD")
        turn=start_review(options,self.backend,ReviewPolicy())
        self.assertTrue(turn.contexts)
        contexts={a["context_id"]:a for a in turn.contexts}
        def response():return dict(binding=turn.binding,candidate_proposals=[],assessments=[],requests=[],finish=False)
        # Fetch remaining pinned source/diff handles through advertised grants.
        for i in range(3):
            grants=[g for g in turn.requests if g["operation"]=="context"]
            if not grants:break
            contexts.update({a["context_id"]:a for a in turn.contexts})
            r=response();r["requests"]=grants[:1]
            turn=advance_review(r,turn.state,self.backend,request_id="context-"+str(i),capability=turn.capability,model_usage=dict(input_tokens=100,output_tokens=10,output_limit=4096))
        contexts.update({a["context_id"]:a for a in turn.contexts})
        source=next(a for a in contexts.values() if a["kind"]=="SOURCE" and any(e["kind"]=="CALLSITE" for e in a["entity_bindings"]))
        call=next(e for e in source["entity_bindings"] if e["kind"]=="CALLSITE")
        p=dict(binding=turn.binding,origin="LLM",claim_kind="copy_bound.v1",anchor_refs=[dict(context_id=source["context_id"],start=call["start"],end=call["end"],entity_ref=None)],contract_refs=[],question="Does this fit?",assumptions=[])
        r=response();r["candidate_proposals"]=[p]
        turn=advance_review(r,turn.state,self.backend,request_id="propose",capability=turn.capability,model_usage=dict(input_tokens=100,output_tokens=10,output_limit=4096))
        r=response();r["requests"]=[next(g for g in turn.requests if g["operation"]=="local-evidence")]
        old=turn
        turn=advance_review(r,turn.state,self.backend,request_id="local",capability=turn.capability,model_usage=dict(input_tokens=100,output_tokens=10,output_limit=4096))
        again=advance_review(r,turn.state,self.backend,request_id="local",capability=old.capability,model_usage=dict(input_tokens=100,output_tokens=10,output_limit=4096))
        self.assertEqual(turn.as_dict(),again.as_dict())
        report=render_report(turn.state)
        extent=next(o["value"]["bytes"] for e in report["local_evidence"] for o in e["observations"] if o["slot"]=="EXTENT")
        self.assertEqual(extent,32)
        a={**p,"claim_kind":"semantic_observation.v1","question":"Queued callbacks require cancellation before releasing state","assumptions":["Single event loop"]}
        r=response();r["candidate_proposals"]=[a];r["finish"]=True
        final=advance_review(r,turn.state,self.backend,request_id="finish",capability=turn.capability,model_usage=dict(input_tokens=100,output_tokens=10,output_limit=4096))
        self.assertEqual(final.status,"CLOSED")
        report=render_report(turn.state);self.assertEqual(report["annotations"][0]["status"],"CONDITIONAL")
        self.assertTrue(all(c["category"]!="VERIFIED FINDING" for c in report["candidates"]))
        self.assertIsNone(report["binding"]["native_binding"])
        # Actual measured cost changes cannot change deterministic semantic JSON.
        other=json.loads(json.dumps(report));other["costs"]["model_input_tokens"]+=1
        self.assertEqual(semantic_report_json(report),semantic_report_json(other))
