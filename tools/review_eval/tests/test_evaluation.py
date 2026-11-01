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

"""Adversarial release-gate tests; all generated responses are test data only."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from review_eval import evaluate, run, report

CORPUS = Path(__file__).resolve().parents[3] / "tests/qualification/review/corpus.json"

class EvaluationTest(unittest.TestCase):
    def setUp(self):
        self.corpus=json.loads(CORPUS.read_text())

    def test_empty_actual_run_stays_open_and_unavailable_is_explicit(self):
        result=evaluate.evaluate(self.corpus, run.pending(self.corpus), [])
        self.assertEqual(result["qualification"], "OPEN")
        self.assertEqual(result["profiles"]["D"]["availability"], "UNAVAILABLE")
        self.assertIsNone(result["profiles"]["A"]["precision"])
        self.assertIn("no attempts", result["gaps"])

    def test_plan_pairs_both_modes_profiles_and_fresh_runs(self):
        jobs=run.schedule(self.corpus, repeats=100, runs=3)
        self.assertEqual(len(jobs), 8100)
        self.assertEqual({j["profile"] for j in jobs}, {"A","B","C"})
        discovery=[j for j in jobs if j["mode"]=="whole_discovery"]
        self.assertEqual(len(discovery), 5400)
        self.assertEqual({j["run_id"] for j in jobs}, {0,1,2})

    def test_p95_requires_one_hundred_real_samples(self):
        self.assertIsNone(evaluate.distribution(list(range(99)))["p95"])
        self.assertEqual(evaluate.distribution(list(range(1,101)))["p95"],95)
        self.assertEqual(evaluate.distribution([])["samples"],0)

    def test_fixed_candidate_success_cannot_hide_discovery_miss(self):
        rows=[{"mode":"fixed_candidate","case_id":"bug","outcome":"TRUE_POSITIVE_SUPPORTED"},
              {"mode":"whole_discovery","case_id":"bug","outcome":"FALSE_NEGATIVE"}]
        metrics=evaluate.quality(rows)
        self.assertEqual(metrics["whole_discovery"]["recall"],0)
        self.assertEqual(metrics["whole_discovery"]["misses"],["bug"])
        self.assertEqual(metrics["fixed_candidate"]["recall"],1)

    def test_all_inconclusive_is_not_deciding(self):
        metrics=evaluate.quality([{"mode":"whole_discovery","case_id":"bug","outcome":"ABSTENTION"}])
        self.assertEqual(metrics["whole_discovery"]["deciding"],0)
        self.assertEqual(metrics["whole_discovery"]["abstention_rate"],1)

    def test_costs_refuse_warm_only_accounting(self):
        errors=evaluate.measurement_gaps({"warm_query_ms":1})
        self.assertIn("provider_bootstrap_ms", errors)
        self.assertIn("cold_setup_ms", errors)
        self.assertIn("aggregate_peak_rss_bytes",errors)

    def test_costs_reject_impossible_totals_negative_and_nan(self):
        data=measurements()
        self.assertEqual(evaluate.measurement_gaps(data),[])
        data["end_to_end_ms"]=0
        self.assertIn("end_to_end_ms below phase total", evaluate.measurement_gaps(data))
        for bad in [-1,float("nan"),float("inf"),True]:
            data=measurements();data["model_ms"]=bad
            self.assertIn("model_ms",evaluate.measurement_gaps(data))

    def test_adjudication_is_bound_to_exact_response_and_separate_reviewer(self):
        attempt={"attempt_id":"one","response_digest":"sha256:"+"a"*64}
        judgment={"attempt_id":"one","response_digest":"sha256:"+"b"*64,
                  "reviewer":"independent","outcome":"TRUE_NEGATIVE"}
        self.assertIn("adjudication response mismatch",evaluate.adjudicate(attempt,judgment,"model")[1])
        judgment["response_digest"]=attempt["response_digest"];judgment["reviewer"]="model"
        self.assertIn("independent adjudicator required",evaluate.adjudicate(attempt,judgment,"model")[1])

    def test_report_has_no_predictions_or_success_for_pending_run(self):
        result=evaluate.evaluate(self.corpus,run.pending(self.corpus),[])
        rendered=report.markdown(result)
        self.assertIn("OPEN",rendered)
        self.assertIn("UNAVAILABLE",rendered)
        self.assertNotIn("P95: 0",rendered)

    def test_adapter_receives_no_oracle_or_held_out_labels(self):
        case=self.corpus["families"][0]["cases"][0]
        job={"case_id":case["case_id"],"profile":"A","mode":"whole_discovery","run_id":0,"sample_id":0}
        request=run.request_for(case,job,{"source":"bounded"},self.corpus)
        serialized=json.dumps(request)
        for field in ["oracle","expected_status","HELD_OUT","harness_id","known_defect"]:
            self.assertNotIn(field,serialized)
        self.assertNotIn("case_id",request)
        self.assertNotIn(case["case_id"],serialized)

    def test_adapter_is_actually_executed_and_output_is_pinned(self):
        with tempfile.TemporaryDirectory() as directory:
            response=run.execute([sys.executable,"-c","import sys,json; x=json.load(sys.stdin); print(json.dumps({'seen':x['hello']}))"],
                                 {"hello":"world"},Path(directory),wall_ms=3000,output_bytes=4096)
            self.assertEqual(response["status"],"EXECUTED")
            self.assertEqual(json.loads((Path(directory)/"response.json").read_text()),{"seen":"world"})
            self.assertTrue(response["response_digest"].startswith("sha256:"))
            self.assertGreater(response["runner_wall_ms"],0)

    def test_adapter_timeout_and_oversize_are_nondeciding(self):
        with tempfile.TemporaryDirectory() as directory:
            for script,expected in [("import time; time.sleep(2)","TIMEOUT"),("print('x'*10000)","OUTPUT_LIMIT")]:
                with self.subTest(expected=expected):
                    result=run.execute([sys.executable,"-c",script],{},Path(directory)/expected,wall_ms=100 if expected=="TIMEOUT" else 3000,output_bytes=256)
                    self.assertEqual(result["status"],expected)

    def test_rejects_mixed_model_policy_and_duplicate_attempts(self):
        bundle=run.pending(self.corpus)
        bundle["attempts"]=[{"attempt_id":"x"},{"attempt_id":"x"}]
        result=evaluate.evaluate(self.corpus,bundle,[])
        self.assertEqual(result["qualification"],"OPEN")
        self.assertTrue(any("duplicate" in gap for gap in result["gaps"]))


    def admitted_fixture(self, directory, *, mode="whole_discovery", outcome="TRUE_POSITIVE_SUPPORTED"):
        # Synthetic transport data only: never a production measurement artifact.
        case=self.corpus["families"][0]["cases"][0]
        bundle=run.pending(self.corpus)
        bundle["configuration"]={"model":"test-model","provider":"test-only","source_disclosure_policy":"test-only"}
        job=dict(case_id=case["case_id"],profile="C",mode=mode,run_id=0,sample_id=0)
        request=run.request_for(case,job,{"source":"test-only"},self.corpus)
        request["configuration"]=bundle["configuration"]
        response={"origin":"PRODUCTION_CONTROLLER","measurements":measurements()}
        code="import json,sys; json.load(sys.stdin); print("+repr(json.dumps(response))+")"
        receipt=run.execute([sys.executable,"-c",code],request,Path(directory),wall_ms=3000,output_bytes=4096)
        attempt=dict(job,attempt_id="test-only",configuration_digest=run.digest(bundle["configuration"]),**receipt)
        bundle["attempts"]=[attempt]
        import hashlib
        raw=(Path(directory)/"response.json").read_bytes()
        judgment=dict(attempt_id="test-only",response_digest=receipt["response_digest"],reviewer="independent-human",
                      outcome=outcome,false_promotion=False,evidence_accurate=True,
                      evidence_refs=[{"path":str(Path(directory)/"response.json"),"digest":"sha256:"+hashlib.sha256(raw).hexdigest()}],
                      checks=["exact_site_refuted","qualified_isolation","base_rejection","boundary_n","short_source","wrong_site","unrelated_crash"])
        return bundle,judgment

    def test_missing_discovery_is_visible_in_release_decision(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle,judgment=self.admitted_fixture(directory,outcome="FALSE_NEGATIVE")
            result=evaluate.evaluate(self.corpus,bundle,[judgment])
            self.assertEqual(result["qualification"],"OPEN")
            self.assertIn("missed or unsupported positive: test-only",result["gaps"])
            self.assertEqual(result["profiles"]["C"]["quality"]["whole_discovery"]["recall"],0)

    def test_all_inconclusive_release_is_open(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle,judgment=self.admitted_fixture(directory,outcome="ABSTENTION")
            result=evaluate.evaluate(self.corpus,bundle,[judgment])
            self.assertIn("missed or unsupported positive: test-only",result["gaps"])
            self.assertEqual(result["profiles"]["C"]["quality"]["whole_discovery"]["deciding"],0)

    def test_single_supported_attempt_does_not_establish_p95_or_full_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle,judgment=self.admitted_fixture(directory)
            result=evaluate.evaluate(self.corpus,bundle,[judgment])
            self.assertEqual(result["admitted_attempts"],1)
            self.assertEqual(result["qualification"],"OPEN")
            self.assertTrue(any("fewer than 100" in g for g in result["gaps"]))
            self.assertTrue(any("fewer than 3" in g for g in result["gaps"]))

    def test_changed_request_or_response_cannot_be_admitted(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle,judgment=self.admitted_fixture(directory)
            (Path(directory)/"response.json").write_text("{}")
            result=evaluate.evaluate(self.corpus,bundle,[judgment])
            self.assertEqual(result["admitted_attempts"],0)
            self.assertIn("test-only: response bytes changed",result["gaps"])

    def test_attempt_nonce_separates_repeated_work_from_duplicate_receipts(self):
        case=self.corpus["families"][0]["cases"][0]
        job=dict(case_id=case["case_id"],profile="C",mode="whole_discovery",run_id=0,sample_id=0)
        first=run.request_for(case,job,{},self.corpus)
        job["sample_id"]=1
        second=run.request_for(case,job,{},self.corpus)
        self.assertNotEqual(run.digest(first),run.digest(second))

    def test_runner_refuses_relative_executable_and_reused_artifact_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                run.execute(["python3","-c","print('{}')"],{},Path(directory),wall_ms=1000,output_bytes=1024)
            artifact=Path(directory)/"attempt"
            run.execute([sys.executable,"-c","print('{}')"],{},artifact,wall_ms=1000,output_bytes=1024)
            with self.assertRaises(ValueError):
                run.execute([sys.executable,"-c","print('{}')"],{},artifact,wall_ms=1000,output_bytes=1024)


    def test_relabeling_one_receipt_cannot_inflate_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle,judgment=self.admitted_fixture(directory)
            duplicate=copy.deepcopy(bundle["attempts"][0]);duplicate["attempt_id"]="reused";duplicate["sample_id"]=1
            bundle["attempts"].append(duplicate)
            j=copy.deepcopy(judgment);j["attempt_id"]="reused"
            result=evaluate.evaluate(self.corpus,bundle,[judgment,j])
            self.assertEqual(result["admitted_attempts"],1)
            self.assertTrue(any("reused" in g for g in result["gaps"]))

    def test_reported_aggregate_memory_uses_observed_process_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle,judgment=self.admitted_fixture(directory)
            observed=bundle["attempts"][0]["aggregate_peak_rss_bytes"]
            result=evaluate.evaluate(self.corpus,bundle,[judgment])
            values=list(result["profiles"]["C"]["workloads"].values())
            self.assertEqual(values[0]["aggregate_peak_rss_bytes"]["min"],observed)

    def test_generic_adapter_assertions_cannot_qualify_production_release(self):
        result=evaluate.evaluate(self.corpus,run.pending(self.corpus),[])
        self.assertIn("production release validator unavailable", result["gaps"])
        self.assertEqual(result["qualification"],"OPEN")
        self.assertEqual(result["evaluation_status"],"INCOMPLETE")

    def test_timeout_kills_compiler_in_separate_process_group(self):
        import os
        import time
        with tempfile.TemporaryDirectory() as directory:
            pid_path=Path(directory)/"child.pid"
            script="import subprocess,os,time; p=subprocess.Popen(['"+sys.executable+"','-c','import time; time.sleep(30)'],process_group=0); open("+repr(str(pid_path))+",'w').write(str(p.pid)); time.sleep(30)"
            result=run.execute([sys.executable,"-c",script],{},Path(directory)/"attempt",wall_ms=1000,output_bytes=1024)
            self.assertEqual(result["status"],"TIMEOUT")
            pid=int(pid_path.read_text())
            # On systems retaining zombies briefly, a zombie is no longer running.
            import subprocess
            try:
                deadline=time.monotonic()+2
                running=True
                while time.monotonic()<deadline:
                    status=subprocess.run(["/bin/ps","-o","stat=","-p",str(pid)],text=True,capture_output=True).stdout.strip()
                    if not status or status.startswith("Z"):running=False;break
                    time.sleep(.01)
                self.assertFalse(running,"separate compiler group survived timeout")
            finally:
                try:os.kill(pid,9)
                except ProcessLookupError:pass


    def test_positive_failures_all_reduce_recall_and_remain_visible(self):
        rows=[dict(mode="whole_discovery",case_id="supported",outcome="TRUE_POSITIVE_SUPPORTED",positive=True),
              dict(mode="whole_discovery",case_id="spurious",outcome="FALSE_POSITIVE",positive=True),
              dict(mode="whole_discovery",case_id="abstained",outcome="ABSTENTION",positive=True)]
        metrics=evaluate.quality(rows)["whole_discovery"]
        self.assertEqual(metrics["recall"],1/3)
        self.assertEqual(metrics["misses"],["abstained","spurious"])

    def test_supported_semantics_are_not_analytically_deciding(self):
        metrics=evaluate.quality([dict(mode="whole_discovery",case_id="callback",positive=True,
                                      outcome="TRUE_POSITIVE_SUPPORTED",analytically_deciding=False)])["whole_discovery"]
        self.assertEqual(metrics["deciding"],0)
        self.assertEqual(metrics["supported_reports"],1)


def measurements():
    return dict(cold_setup_ms=1,cold_index_ms=2,provider_bootstrap_ms=3,model_ms=4,
                warm_query_ms=5,end_to_end_ms=15,aggregate_peak_rss_bytes=100,
                physical_footprint_bytes=100,live_store_bytes=30,physical_store_bytes=40,
                model_turns=1,input_tokens=2,output_tokens=3,model_cost_usd=0.01,
                reviewer_ms=10,load_average=1,swap_bytes=0)
