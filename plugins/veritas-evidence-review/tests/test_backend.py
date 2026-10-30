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



import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"scripts"))
from veritas_review.backend import Backend, ProcessLimits, BackendError
from veritas_review import review_types as t

class BackendTransportTest(unittest.TestCase):
    def worker(self, code, limits=None):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        script=Path(temp.name)/"worker.py";script.write_text(code)
        backend=Backend((sys.executable,"-I",str(script)),cwd=temp.name,limits=limits or ProcessLimits())
        self.addCleanup(backend.close);return backend

    def test_InvalidOperationFailsBeforeSpawn(self):
        backend=Backend(("/does/not/exist",),cwd="/tmp")
        self.addCleanup(backend.close)
        with self.assertRaises(ValueError):backend.call("shell",{})
        self.assertIsNone(backend.pid)

    def test_OutputOverflowKillsProcessGroup(self):
        backend=self.worker("import sys\nsys.stdin.readline()\nsys.stdout.write('x'*1000000)\nsys.stdout.flush()",ProcessLimits(output_bytes=1024))
        with self.assertRaisesRegex(BackendError,"stdout limit"):backend.handshake()
        self.assertIsNone(backend.pid)

    def test_StderrFloodAndTimeout(self):
        for expected,code in [("stderr limit","import sys\nsys.stdin.readline()\nsys.stderr.write('x'*1000000)\nsys.stderr.flush()"), ("deadline", "import time\ntime.sleep(30)")]:
            backend=self.worker(code,ProcessLimits(wall_ms=150 if expected=="deadline" else 2000,stderr_bytes=1024))
            start=time.monotonic()
            with self.assertRaisesRegex(BackendError,expected):backend.handshake()
            self.assertLess(time.monotonic()-start,3)
            self.assertIsNone(backend.pid)

    def test_WrongRequestIdAndUnadvertisedOperation(self):
        response=t.canonical_json({"protocol":t.PROTOCOL,"request_id":"wrong","ok":True,
          "result":{"protocols":[t.PROTOCOL],"backend_version":"test","capabilities":[]}})
        backend=self.worker("import sys\nsys.stdin.readline()\nsys.stdout.buffer.write("+repr(response+b"\n")+")\nsys.stdout.flush()")
        with self.assertRaises(BackendError):backend.handshake()
        with self.assertRaises(BackendError):backend.call("capture",{})

    def test_DescendantHoldingStdoutIsKilledOnDeadline(self):
        backend=self.worker("import os,time,sys\nsys.stdin.readline()\nif os.fork()==0: time.sleep(30)\nelse: os._exit(0)",ProcessLimits(wall_ms=150))
        with self.assertRaisesRegex(BackendError,"deadline"):backend.handshake()
        self.assertIsNone(backend.pid)

    def test_NonpositiveAndBooleanLimitsAreRejected(self):
        for value in [0,-1,True]:
            with self.assertRaises(ValueError):ProcessLimits(wall_ms=value)

    def test_HardIsolationRefusesBeforeLaunch(self):
        backend=self.worker("raise RuntimeError('must not execute')",ProcessLimits(hard_isolation=True))
        with self.assertRaisesRegex(BackendError,"UNSUPPORTED"):backend.handshake()
        self.assertIsNone(backend.pid)

    def test_EnvironmentIsScrubbedAndAggregateDeadlineIsShared(self):
        response={"protocol":t.PROTOCOL,"ok":True,"result":{"protocols":[t.PROTOCOL],"backend_version":"test","capabilities":[]}}
        code="import json,sys,os,time\nfor line in sys.stdin:\n q=json.loads(line)\n time.sleep(0.08)\n assert 'VERITAS_TEST_SECRET' not in os.environ\n r="+repr(response)+"\n r['request_id']=q['request_id']\n print(json.dumps(r),flush=True)"
        os.environ["VERITAS_TEST_SECRET"]="test-only"
        self.addCleanup(os.environ.pop,"VERITAS_TEST_SECRET",None)
        backend=self.worker(code,ProcessLimits(wall_ms=1000,aggregate_wall_ms=120))
        backend.handshake()
        with self.assertRaisesRegex(BackendError,"deadline"):backend.handshake()

    def test_DescendantCannotWriteAfterGroupTermination(self):
        with tempfile.TemporaryDirectory() as root:
            ready=Path(root)/"ready";canary=Path(root)/"canary"
            code="import os,time,sys\nsys.stdin.readline()\nif os.fork()==0:\n open("+repr(str(ready))+",'w').close()\n time.sleep(0.5)\n open("+repr(str(canary))+",'w').close()\nelse: os._exit(0)"
            backend=self.worker(code,ProcessLimits(wall_ms=200))
            with self.assertRaisesRegex(BackendError,"deadline"):backend.handshake()
            self.assertTrue(ready.exists())
            time.sleep(0.55)
            self.assertFalse(canary.exists())

    def test_SeparateCompilerProcessGroupIsTerminated(self):
        with tempfile.TemporaryDirectory() as root:
            ready=Path(root)/"ready";canary=Path(root)/"canary"
            code="import os,time,sys\nsys.stdin.readline()\nif os.fork()==0:\n os.setpgid(0,0)\n open("+repr(str(ready))+",'w').close()\n time.sleep(1)\n open("+repr(str(canary))+",'w').close()\n os._exit(0)\nelse: time.sleep(30)"
            backend=self.worker(code,ProcessLimits(wall_ms=400))
            with self.assertRaisesRegex(BackendError,"deadline"):backend.handshake()
            self.assertTrue(ready.exists())
            time.sleep(1.1)
            self.assertFalse(canary.exists(),"compiler group survived its backend supervisor")

    def test_ReleaseDoesNotInventCaptureCapability(self):
        from test_verification import fixture
        binding=fixture()[0]["binding"]
        capability=lambda op:{"operation":op,"version":"2","predicate_kinds":[],"authority":"OBSERVATION",
           "limits":{"wall_ms":1000,"output_bytes":1048576,"examined":100}}
        coverage={"supported":True,"scope_closed":False,"traversal":"COMPLETE","frontier_refs":[],"reasons":[],"examined":0}
        results=[{"protocols":[t.PROTOCOL],"backend_version":"test","capabilities":[capability("capture")]},
          {"binding":binding,"context_refs":[],"capabilities":[capability("release")],"coverage":coverage},
          {"binding":binding,"released":True}]
        code="import json,sys\nresults="+repr(results)+"\nfor result in results:\n q=json.loads(sys.stdin.readline())\n print(json.dumps({'protocol':"+repr(t.PROTOCOL)+",'request_id':q['request_id'],'ok':True,'result':result}),flush=True)"
        backend=self.worker(code)
        backend.handshake()
        parameters={"project_key":"fixture","base_ref":"HEAD","target_ref":None,"selected_untracked":[],"program_manifest_key":None,"context_selection_keys":[]}
        backend.call("capture",parameters);backend.call("release",{"binding":binding})
        with self.assertRaisesRegex(BackendError,"not advertised"):backend.call("capture",parameters)

    def test_EchoedRequestIsNotAResponseAndClosesSession(self):
        backend=self.worker("import sys,time\nline=sys.stdin.buffer.readline()\nsys.stdout.buffer.write(line)\nsys.stdout.flush()\ntime.sleep(30)")
        with self.assertRaisesRegex(BackendError,"response envelope"):backend.handshake()
        self.assertIsNone(backend.pid)
