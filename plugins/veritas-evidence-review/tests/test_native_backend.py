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
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"scripts"))
from veritas_review.backend import Backend, BackendError, ProcessLimits
from veritas_review import review_types as t

class NativeBackendTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix="rdx07-native-");self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name).resolve()/"project";self.root.mkdir()
        self.binary=Path(os.environ.get("VERITAS_REVIEW_BACKEND",str(Path(__file__).resolve().parents[3]/"build-homebrew-clang/bin/veritas-review-backend")))
        self.assertTrue(self.binary.is_file(),"build veritas-review-backend before running integration tests")
        self.source="extern \"C\" void *memcpy(void *, const void *, unsigned long);\nvoid f(const char *p, unsigned long n) { char b[32]; memcpy(b,p,n); }\n"
        (self.root/"main.cc").write_text(self.source)
        (self.root/"compile_commands.json").write_text(json.dumps([{"directory":str(self.root),"file":"main.cc",
           "arguments":["clang++","--target=x86_64-unknown-linux-gnu","-std=c++20","-c","main.cc"]}]))
        self.git("init","-q");self.git("add",".");self.git("-c","user.name=Test","-c","user.email=test@example.invalid","commit","-qm","base")
        (self.root/"main.cc").write_text(self.source+"// dirty before capture\n")
        self.config={"project_key":"fixture","project_root":str(self.root),"repository_id":"repo:sha256:"+"a"*64,
          "base_ref":"HEAD","compile_commands_path":"compile_commands.json","selected_tracked":[],
          "selected_artifacts":[],"allowed_untracked":[],"context_selections":{},"programs":{},
          "limits":{"wall_ms":10000,"output_bytes":1048576,"examined":10000,"context_bytes":32768,"request_bytes":1048576,"requests":256}}
        self.config_path=Path(self.temp.name)/"operator.json";self.config_path.write_text(json.dumps(self.config))
        self.backend=Backend((str(self.binary),"handshake","--config",str(self.config_path)),cwd=self.temp.name,
          limits=ProcessLimits(wall_ms=10000))
        self.addCleanup(self.backend.close)

    def git(self,*args):
        subprocess.run(("/usr/bin/git","-C",str(self.root),*args),check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)

    def capture(self):
        handshake=t._wire(self.backend.handshake().result)
        self.assertFalse(any(x["operation"] in {"verify","native-evidence"} for x in handshake["capabilities"]))
        result=t._wire(self.backend.call("capture",{"project_key":"fixture","base_ref":"HEAD","target_ref":None,
          "selected_untracked":[],"program_manifest_key":None,"context_selection_keys":[]}).result)
        self.assertIsNone(result["binding"]["native_binding"])
        return result

    def context(self,capture,refs):
        return t._wire(self.backend.call("context",{"binding":capture["binding"],"context_refs":refs,"max_bytes":32768,"max_tokens":32768}).result)

    def test_ContextUsesAdvertisedSnapshotAndRealLocalEvidence(self):
        captured=self.capture()
        (self.root/"main.cc").write_text("after capture — no copy\n")
        contexts=[]
        for ref in captured["context_refs"]:
            contexts.extend(self.context(captured,[ref])["artifacts"])
        source=next(c for c in contexts if c["kind"]=="SOURCE" and c["entity_bindings"])
        self.assertIn("dirty before capture",source["excerpt"])
        self.assertNotIn("after capture",source["excerpt"])
        self.assertTrue(any(c["kind"]=="DIFF" for c in contexts))
        call=next(e["entity_ref"] for e in source["entity_bindings"] if e["kind"]=="CALLSITE")
        evidence=t._wire(self.backend.call("local-evidence",{"binding":captured["binding"],"predicate_kind":"copy_bound.v1",
            "callsite_ref":call,"requested_slots":["EXTENT","GUARD","OPERANDS"],"max_examined":100}).result)
        self.assertEqual(next(o["value"]["bytes"] for o in evidence["observations"] if o["slot"]=="EXTENT"),32)
        self.assertIsNone(evidence["binding"]["native_binding"])
        seeds=t._wire(self.backend.call("seed-candidates",{"binding":captured["binding"],
             "changed_context_refs":[source["context_id"]],"limit":16}).result)
        self.assertEqual(len(seeds["candidates"]),1)
        self.assertEqual(seeds["candidates"][0]["anchor_refs"][0]["entity_ref"],call)
        self.assertGreater(self.backend.last_usage["rss_samples"],0)
        self.backend.call("release",{"binding":captured["binding"]})
        with self.assertRaises(BackendError):self.context(captured,[source["context_id"]])

    def test_WrongBindingAndUnadvertisedOperationsFailBeforeDispatch(self):
        captured=self.capture();pid=self.backend.pid;usage=self.backend.last_usage.copy()
        bad=copy.deepcopy(captured);bad["binding"]["working_tree_digest"]="b"*64
        with self.assertRaises(ValueError):self.context(bad,[])
        self.assertEqual(self.backend.pid,pid);self.assertEqual(self.backend.last_usage,usage)
        for operation in ["native-evidence","verify"]:
            with self.assertRaisesRegex(BackendError,"not advertised"):self.backend.call(operation,{})
        self.assertEqual(self.backend.pid,pid)

    def test_UnadvertisedContextAndOutputLimitsPreserveSnapshot(self):
        captured=self.capture()
        with self.assertRaisesRegex(BackendError,"INVALID_REFERENCE"):
            self.context(captured,[t.artifact_id(t.ContextId,{"invented":1}).value])
        self.assertIsNotNone(self.backend.pid)
        good=self.context(captured,[captured["context_refs"][0]])
        self.assertEqual(good["binding"],captured["binding"])
        with self.assertRaisesRegex(BackendError,"LIMIT_EXCEEDED"):
            self.backend.call("context",{"binding":captured["binding"],"context_refs":[captured["context_refs"][0]],"max_bytes":1,"max_tokens":1})
        self.assertEqual(self.context(captured,[])["binding"],captured["binding"])

    def test_NoCapabilityDoesNotSelectPractice(self):
        self.backend.handshake()
        with self.assertRaisesRegex(BackendError,"INVALID_SELECTION"):
            self.backend.call("capture",{"project_key":"wrong","base_ref":"HEAD","target_ref":None,
              "selected_untracked":[],"program_manifest_key":None,"context_selection_keys":[]})
        self.assertIsNotNone(self.backend.pid)

    def test_V1ClientCannotReadV2LocalEvidence(self):
        request={"protocol":"veritas.review.backend.v1","request_id":"v1","operation":"handshake","parameters":{"accepted_protocols":["veritas.review.backend.v1"]}}
        result=subprocess.run((str(self.binary),"handshake"),input=json.dumps(request).encode(),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        self.assertNotEqual(result.returncode,0);self.assertEqual(result.stdout,b"")

    def test_LoweredWorkLimitRefusesCapture(self):
        self.config["limits"]["examined"]=1
        self.config_path.write_text(json.dumps(self.config))
        self.backend.handshake()
        with self.assertRaisesRegex(BackendError,"LIMIT_EXCEEDED"):
            self.backend.call("capture",{"project_key":"fixture","base_ref":"HEAD","target_ref":None,
                "selected_untracked":[],"program_manifest_key":None,"context_selection_keys":[]})

    def test_DirectNativeAndVerifyRefusalsAreBoundAndAtomic(self):
        import test_verification
        process=subprocess.Popen((str(self.binary),"capture","--config",str(self.config_path)),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        self.addCleanup(lambda:process.poll() is None and process.kill())
        counter=0
        def call(operation,parameters):
            nonlocal counter
            counter+=1;identity="direct-"+str(counter)
            process.stdin.write(t.canonical_json({"protocol":t.PROTOCOL,"request_id":identity,"operation":operation,"parameters":parameters})+b"\n")
            process.stdin.flush()
            response=t.decode_v2(process.stdout.readline(),expected_request_id=identity,expected_operation=operation)
            return response
        try:
            self.assertTrue(call("handshake",{"accepted_protocols":[t.PROTOCOL]}).ok)
            captured=t._wire(call("capture",{"project_key":"fixture","base_ref":"HEAD","target_ref":None,
               "selected_untracked":[],"program_manifest_key":None,"context_selection_keys":[]}).result)
            binding=captured["binding"]
            native={"binding":binding,"candidate_id":t.artifact_id(t.CandidateId,{}).value,"level":"L0",
              "budget":{k:1 for k in ["max_depth","max_nodes","max_paths","max_facts_per_query","max_provenance_depth"]}}
            self.assertEqual(call("native-evidence",native).error.code,"UNSUPPORTED")
            request,_,_=test_verification.fixture();request["binding"]=binding
            request["execution_domain"]["snapshot_id"]=binding["snapshot_id"]
            request["obligation_id"]=test_verification.v.obligation_id(request)
            self.assertEqual(call("verify",request).error.code,"UNSUPPORTED")
            wrong=copy.deepcopy(binding);wrong["working_tree_digest"]="b"*64
            self.assertEqual(call("context",{"binding":wrong,"context_refs":[],"max_bytes":1,"max_tokens":1}).error.code,"BINDING_MISMATCH")
            context=call("context",{"binding":binding,"context_refs":[captured["context_refs"][0]],"max_bytes":32768,"max_tokens":32768})
            self.assertTrue(context.ok)
            self.assertEqual(t._wire(context.result)["binding"],binding)
            self.assertTrue(call("release",{"binding":binding}).ok)
            self.assertEqual(call("release",{"binding":binding}).error.code,"BINDING_MISMATCH")
        finally:
            process.stdin.close();process.wait(timeout=2)
            process.stdout.close();process.stderr.close()

    def test_RehandshakeRetainsActiveSnapshotCapabilities(self):
        captured=self.capture()
        self.backend.handshake()
        self.assertEqual(self.context(captured,[])["binding"],captured["binding"])
        self.assertTrue(self.backend.call("release",{"binding":captured["binding"]}).result.released)
