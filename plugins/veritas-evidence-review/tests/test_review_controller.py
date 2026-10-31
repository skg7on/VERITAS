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
import json
import sys
import tempfile
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"scripts"))
from veritas_review.controller import start_review, advance_review, recover_review
from veritas_review.models import ReviewOptions, ReviewPolicy
from veritas_review.reporting import render_report, render_markdown
from test_candidates import context, proposal
from test_review_types import binding

# Specific boundary double for crash and budget tests. Native integration tests
# exercise the same controller against the resident C++ service.
class Service:
    def __init__(self):self.calls=[];self.last_usage={"wall_ms":1};self.fail=False
    def call(self, op, params, binding=None):
        self.calls.append((op,copy.deepcopy(params)))
        cap=lambda op:dict(operation=op,version="1",predicate_kinds=["copy_bound.v1"],authority="OBSERVATION",limits=dict(wall_ms=10,output_bytes=10000,examined=100))
        cov=dict(supported=True,scope_closed=False,traversal="COMPLETE",frontier_refs=[],reasons=[],examined=1)
        if op=="handshake":return dict(protocols=["veritas.review.backend.v2"],backend_version="1",capabilities=[cap(x) for x in ("capture","context","seed-candidates","local-evidence","release")])
        if op=="capture":return dict(binding=globals()["binding"](),context_refs=[context()["context_id"]],capabilities=[cap(x) for x in ("context","seed-candidates","local-evidence","release")],coverage=cov)
        if op=="context":return dict(binding=globals()["binding"](),artifacts=[context()],usage=dict(wall_ms=1,cpu_ms=0,peak_rss_bytes=0,output_bytes=100,examined=1),coverage=cov)
        if op=="seed-candidates":return dict(binding=globals()["binding"](),candidates=[proposal("DETERMINISTIC")],coverage=cov,usage=dict(wall_ms=1,cpu_ms=0,peak_rss_bytes=0,output_bytes=100,examined=1))
        if op=="local-evidence":
            if self.fail:raise RuntimeError("simulated interruption")
            return dict(local_evidence_id="local-evidence:sha256:"+"f"*64,binding=globals()["binding"](),predicate_kind="copy_bound.v1",callsite_ref=params["callsite_ref"],observations=[],assumptions=[],producer=dict(name="local",version="1",config_digest="a"*64),coverage=cov,usage=dict(wall_ms=1,cpu_ms=0,peak_rss_bytes=0,output_bytes=100,examined=1))
        if op=="release":return dict(binding=globals()["binding"](),released=True)
        raise AssertionError(op)
    def close(self):pass

class ControllerTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.backend=Service()
        self.options=ReviewOptions(state_dir=Path(self.temp.name)/"review",project_key="p",base_ref="HEAD")
        self.turn=start_review(self.options,self.backend,ReviewPolicy())
        self.state=self.turn.state
    def response(self, **kw):
        return dict(binding=binding(),candidate_proposals=[],assessments=[],requests=[],finish=False,**kw)
    def advance(self, response, request_id="turn-1", **kw):
        return advance_review(response,self.state,self.backend,request_id=request_id,capability=self.turn.capability,
            model_usage={"input_tokens":100,"output_tokens":100,"output_limit":4096},**kw)

    def test_pre_eir_context_and_duplicate_admission(self):
        self.assertIsNone(self.turn.binding["native_binding"])
        self.assertEqual(len(self.turn.contexts),1)
        response=self.response();response["candidate_proposals"]=[proposal()]
        self.advance(response)
        report=render_report(self.state)
        self.assertEqual(len(report["candidates"]),1)
        self.assertEqual(report["candidates"][0]["origins"],["DETERMINISTIC","LLM"])

    def test_authority_unknown_fields_stale_and_invented_refs_no_dispatch(self):
        for change in (lambda r:r.update(authority="MUST"), lambda r:r["binding"].update(working_tree_digest="b"*64),
            lambda r:r["assessments"].append(dict(candidate_id="candidate:sha256:"+"f"*64,assessment="VERIFIED_SAFE",refs=[],assumptions=[])),
            lambda r:r["requests"].append(dict(request_id="fake",operation="shell",parameters={}))):
            r=self.response();change(r);n=len(self.backend.calls)
            with self.assertRaises(ValueError):self.advance(r)
            self.assertEqual(len(self.backend.calls),n)

    def test_identical_request_retry_replays_after_recovery(self):
        r=self.response();r["requests"]=[next(g for g in self.turn.requests if g["operation"]=="local-evidence")]
        first=self.advance(r);n=len(self.backend.calls)
        recovered=recover_review(self.options.state_dir)
        replay=advance_review(r,recovered,self.backend,request_id="turn-1",capability=self.turn.capability,
            model_usage={"input_tokens":100,"output_tokens":100,"output_limit":4096})
        self.assertEqual(first.as_dict(),replay.as_dict());self.assertEqual(len(self.backend.calls),n)
        r["finish"]=True
        with self.assertRaises(ValueError):self.advance(r)

    def test_capability_reuse_other_request_rejected(self):
        self.advance(self.response())
        with self.assertRaises(ValueError):self.advance(self.response(),request_id="turn-2")

    def test_oversized_response_no_backend(self):
        r=self.response();r["candidate_proposals"]=[proposal()];r["candidate_proposals"][0]["question"]="x"*65536
        n=len(self.backend.calls)
        with self.assertRaises(ValueError):self.advance(r)
        self.assertEqual(len(self.backend.calls),n)

    def test_interrupted_dispatch_is_never_reexecuted(self):
        self.backend.fail=True;r=self.response();r["requests"]=[next(g for g in self.turn.requests if g["operation"]=="local-evidence")]
        with self.assertRaises(RuntimeError):self.advance(r)
        n=len(self.backend.calls)
        recovered=recover_review(self.options.state_dir)
        with self.assertRaisesRegex(ValueError,"interrupted|ABORTED"):advance_review(r,recovered,self.backend,request_id="turn-1",capability=self.turn.capability,model_usage={"input_tokens":100,"output_tokens":100,"output_limit":4096})
        self.assertEqual(len(self.backend.calls),n)
        self.assertEqual(render_report(recovered)["status"],"ABORTED")

    def test_semantic_annotation_finish_and_escaped_markdown(self):
        p=proposal(kind="semantic_observation.v1");p["question"]="<script> callback [run](https://bad)"
        r=self.response();r["candidate_proposals"]=[p];r["finish"]=True
        turn=self.advance(r);report=render_report(self.state)
        self.assertEqual(turn.status,"CLOSED")
        self.assertEqual(report["annotations"][0]["status"],"PROPOSED")
        self.assertEqual(report["annotations"][0]["origin"],"LLM_INFERRED")
        self.assertNotIn("<script>",render_markdown(report));self.assertNotIn("](https://bad)",render_markdown(report))
        self.assertIn("No safety conclusion",render_markdown(report))

    def test_audit_tampering_refuses_recovery(self):
        path=self.options.state_dir/"checkpoint.json";raw=json.loads(path.read_text());raw["state"]["status"]="CLOSED";path.write_text(json.dumps(raw))
        with self.assertRaises(ValueError):recover_review(self.options.state_dir)

    def test_missing_model_usage_stops_followup(self):
        turn=advance_review(self.response(),self.state,self.backend,request_id="no-usage",capability=self.turn.capability,model_usage=None)
        self.assertEqual(turn.status,"REPORT_READY");self.assertIsNone(turn.capability)

    def test_context_input_is_charged_conservatively(self):
        self.advance(self.response())
        costs=render_report(self.state)["costs"]
        self.assertGreater(costs["model_input_tokens"],100)

    def test_model_turn_limit_never_returns_ninth_capability(self):
        turn=self.turn
        for i in range(8):
            turn=advance_review(self.response(),self.state,self.backend,request_id="r"+str(i),capability=turn.capability,
                model_usage={"input_tokens":1,"output_tokens":1,"output_limit":4096})
        self.assertEqual(turn.status,"REPORT_READY");self.assertIsNone(turn.capability)
        with self.assertRaises(ValueError):advance_review(self.response(),self.state,self.backend,request_id="ninth",capability="a"*64,model_usage={"input_tokens":1,"output_tokens":1,"output_limit":4096})

    def test_rejected_candidates_remain_in_coverage(self):
        r=self.response();p=proposal();p["anchor_refs"][0]["entity_ref"]={"index_id":"structural-index:sha256:"+"a"*64,"ordinal":999}
        r["candidate_proposals"]=[p];self.advance(r)
        report=render_report(self.state)
        self.assertEqual(len(report["coverage"]["rejected"]),1)
        self.assertEqual(len(report["candidates"]),1)

    def test_turn_tokens_are_not_present_at_rest(self):
        for path in self.options.state_dir.iterdir():
            if path.is_file():self.assertNotIn(self.turn.capability.encode(),path.read_bytes())

    def test_invalid_responses_still_consume_model_budget(self):
        from veritas_review.audit import AuditStore
        store=AuditStore(self.options.state_dir)
        with store.locked():
            data=store.load();data["policy"]["input_tokens"]=100000;store.save(data,"test-isolate-turn-ceiling")
        for i in range(8):
            r=self.response();r["authority"]="VERIFIED_SAFE"
            with self.assertRaises(ValueError):self.advance(r,request_id="bad-"+str(i))
        report=render_report(self.state)
        self.assertEqual(report["costs"]["model_turns"],8)
        self.assertEqual(report["status"],"REPORT_READY")

    def test_no_multiple_context_windows_in_one_turn(self):
        from veritas_review.audit import AuditStore
        store=AuditStore(self.options.state_dir)
        with store.locked():
            data=store.load()
            data["context_refs"]=["context:sha256:"+c*64 for c in "bc"]
            data["grants"]=[]
            from veritas_review.controller import _grants
            data["grants"]=_grants(data);store.save(data,"test-context-selection")
        r=self.response();r["requests"]=data["grants"]
        n=len(self.backend.calls)
        with self.assertRaises(ValueError):self.advance(r)
        self.assertEqual(len(self.backend.calls),n)

    def test_crash_after_backend_completion_does_not_redispatch(self):
        from unittest.mock import patch
        from veritas_review.audit import AuditStore
        original=AuditStore.save
        def crash(store,data,event):
            original(store,data,event)
            if event=="backend-completed" and data["pending_turn"] is not None:
                raise RuntimeError("crash after completion")
        r=self.response();r["requests"]=[next(g for g in self.turn.requests if g["operation"]=="local-evidence")]
        with patch.object(AuditStore,"save",crash),self.assertRaisesRegex(RuntimeError,"after completion"):
            self.advance(r)
        n=len(self.backend.calls);recovered=recover_review(self.options.state_dir)
        self.assertEqual(render_report(recovered)["status"],"ABORTED")
        with self.assertRaises(ValueError):advance_review(r,recovered,self.backend,request_id="turn-1",capability=self.turn.capability,model_usage={"input_tokens":100,"output_tokens":100,"output_limit":4096})
        self.assertEqual(len(self.backend.calls),n)

    def test_concurrent_identical_retry_has_one_execution(self):
        from concurrent.futures import ThreadPoolExecutor
        r=self.response();r["requests"]=[next(g for g in self.turn.requests if g["operation"]=="local-evidence")]
        n=len(self.backend.calls)
        with ThreadPoolExecutor(max_workers=2) as pool:
            futures=[pool.submit(self.advance,r) for _ in range(2)]
            left,right=[f.result().as_dict() for f in futures]
        self.assertEqual(left,right);self.assertEqual(len(self.backend.calls),n+1)

    def test_source_context_fetch_runs_deterministic_admission(self):
        from veritas_review.audit import AuditStore
        from veritas_review.controller import _grants
        store=AuditStore(self.options.state_dir)
        with store.locked():
            data=store.load();data["contexts"]={};data["candidates"]={};data["grants"]=_grants(data);store.save(data,"test-context-not-yet-fetched")
        r=self.response();r["requests"]=[data["grants"][0]]
        self.advance(r)
        self.assertEqual(len(render_report(self.state)["candidates"]),1)

    def test_unhashable_model_assessment_is_charged_and_rejected(self):
        cid=self.turn.as_dict()["candidates"][0]["candidate_id"]
        r=self.response();r["assessments"]=[dict(candidate_id=cid,assessment=[],refs=[],assumptions=[])]
        n=len(self.backend.calls)
        with self.assertRaises(ValueError):self.advance(r)
        self.assertEqual(render_report(self.state)["costs"]["model_turns"],1)
        with self.assertRaises(ValueError):self.advance(r)
        self.assertEqual(render_report(self.state)["costs"]["model_turns"],1)
        self.assertEqual(len(self.backend.calls),n)

    def test_audit_exhaustion_preserves_bounded_abort_recovery(self):
        options=ReviewOptions(state_dir=Path(self.temp.name)/"small-audit",project_key="p",base_ref="HEAD")
        backend=Service();turn=start_review(options,backend,ReviewPolicy(audit_bytes=50000))
        r=self.response();r["requests"]=[next(g for g in turn.requests if g["operation"]=="local-evidence")]
        with self.assertRaisesRegex(ValueError,"audit byte budget"):
            advance_review(r,turn.state,backend,request_id="bounded",capability=turn.capability,model_usage={"input_tokens":100,"output_tokens":100,"output_limit":4096})
        recovered=recover_review(options.state_dir)
        self.assertEqual(render_report(recovered)["status"],"ABORTED")
        self.assertLessEqual((options.state_dir/"events.jsonl").stat().st_size,50000)

    def test_unvisited_context_is_recorded_when_finishing(self):
        from veritas_review.audit import AuditStore
        ref="context:sha256:"+"b"*64;store=AuditStore(self.options.state_dir)
        with store.locked():
            data=store.load();data["context_refs"].append(ref);store.save(data,"test-advertised-unvisited")
        r=self.response();r["finish"]=True;self.advance(r)
        report=render_report(self.state)
        self.assertTrue(any(d.get("context_id")==ref for d in report["coverage"]["deferred"]))

    def test_model_cannot_forge_deterministic_origin(self):
        r=self.response();r["candidate_proposals"]=[proposal("DETERMINISTIC",kind="semantic_observation.v1")]
        n=len(self.backend.calls)
        with self.assertRaises(ValueError):self.advance(r)
        report=render_report(self.state)
        self.assertFalse(report["annotations"]);self.assertEqual(len(self.backend.calls),n)

    def test_audit_exhaustion_while_rejecting_also_stops_attempt(self):
        options=ReviewOptions(state_dir=Path(self.temp.name)/"reject-audit",project_key="p",base_ref="HEAD")
        backend=Service();turn=start_review(options,backend,ReviewPolicy(audit_bytes=50000))
        r=self.response();r["authority"]="MUST"
        for i in range(5):
            with self.assertRaises(ValueError):advance_review(r,turn.state,backend,request_id="bad-"+str(i),capability=turn.capability,model_usage={"input_tokens":100,"output_tokens":100,"output_limit":4096})
        self.assertEqual(render_report(recover_review(options.state_dir))["status"],"ABORTED")
