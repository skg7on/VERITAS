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


import dataclasses
import json
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from veritas_review.review_types import (decode_v2, canonical_json, artifact_id,
    ReviewBinding, ContextId, SnapshotId, ProgramScope)

P = "veritas.review.backend.v2"
def ident(kind):
    return kind + ":sha256:" + "a" * 64

def binding():
    return dict(snapshot_id=ident("review-snapshot"), repository_id="repo:sha256:"+"b"*64,
                base_revision_id="rev:sha256:"+"c"*64, working_tree_digest="d"*64,
                program_scope_id=ident("program-scope"), build_context_digest="e"*64,
                native_binding=None, provider_versions=[])

def request(op, params):
    return dict(protocol=P, request_id="r1", operation=op, parameters=params)

def decode(value, **kwargs):
    return decode_v2(json.dumps(value).encode(), **kwargs)

class ReviewTypesTest(unittest.TestCase):
    def test_LocalBindingHasNoNativeRun(self):
        b = ReviewBinding.from_wire(binding())
        self.assertIsNone(b.native_binding)
        self.assertIsInstance(b.snapshot_id, SnapshotId)
        with self.assertRaises(dataclasses.FrozenInstanceError):
            b.working_tree_digest = "new"
        self.assertEqual(decode(request("release", {"binding":binding()})).parameters.binding, b)

    def test_RejectsDuplicateAndUnknownKeys(self):
        for raw in [b'{"protocol":"veritas.review.backend.v2","protocol":"v1"}',
                    b'{"x":{"a":1,"a":2}}', b'{"x":NaN}', b'{}{}', b'\xff',
                    b'{"x":"\\ud800"}', b'{"x":1.0}', b'{"x":1e0}']:
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                decode_v2(raw)
        bad = request("handshake", {"accepted_protocols":[P], "authority":"FORMAL"})
        with self.assertRaises(ValueError): decode(bad)
        b = binding(); b["epistemic_state"]="MUST"
        with self.assertRaises(ValueError): decode(request("release", {"binding":b}))

    def test_ArtifactKindsCannotBeSubstituted(self):
        with self.assertRaises(ValueError): SnapshotId(ident("context"))
        b = binding(); b["snapshot_id"]=ident("context")
        with self.assertRaises(ValueError): ReviewBinding.from_wire(b)
        self.assertNotEqual(ContextId(ident("context")), SnapshotId(ident("review-snapshot")))

    def test_CanonicalIdsIgnoreAuditTiming(self):
        semantic = {"n":18446744073709551615,"text":"hé\n"}
        self.assertEqual(canonical_json(semantic), b'{"n":18446744073709551615,"text":"h\xc3\xa9\\n"}')
        self.assertEqual(artifact_id(ContextId, semantic, audit={"wall_ms":1}),
                         artifact_id(ContextId, semantic, audit={"wall_ms":999}))
        self.assertNotEqual(artifact_id(ContextId, semantic), artifact_id(ContextId, {**semantic, "program":"other"}))
        self.assertNotEqual(artifact_id(ContextId, semantic), artifact_id(ContextId, {**semantic, "config":"other"}))
        self.assertEqual(artifact_id(ContextId, semantic).value, "context:sha256:1b7b242e87e11268112f4919a50c07e5b7dc8e3ce15b4700646fa7d7d2a33bdf")
        self.assertEqual(canonical_json({"z":None,"a":"\x01/é","n":-9223372036854775808}),
                         b'{"a":"\\u0001/\xc3\xa9","n":-9223372036854775808,"z":null}')

    def test_HandshakeHasNoBinding(self):
        env = decode(request("handshake", {"accepted_protocols":[P]}))
        self.assertFalse(hasattr(env.parameters, "binding"))
        v1 = request("handshake", {"accepted_protocols":[P]}); v1["protocol"]="veritas.review.backend.v1"
        with self.assertRaises(ValueError): decode(v1)
        with self.assertRaises(ValueError): decode(request("handshake", {"accepted_protocols":[P],"binding":None}))
        with self.assertRaises(ValueError): decode(request("handshake", {"accepted_protocols":[P]}), expected_request_id="wrong")

    def test_CaptureUsesTrustedSelectionKeysBeforeRefs(self):
        params = dict(project_key="project", base_ref="HEAD",target_ref=None,
                      selected_untracked=["new.cc"],program_manifest_key="program",context_selection_keys=["docs"])
        self.assertEqual(decode(request("capture", params)).parameters.program_manifest_key,"program")
        for key, value in [("program_manifest_key","/tmp/p.json"),("program_manifest_key",ident("context")),
                           ("project_key","../escape"),("base_ref","--upload-pack=evil"),("selected_untracked",["../secret"])]:
            with self.subTest(key=key), self.assertRaises(ValueError): decode(request("capture", {**params,key:value}))

    def test_NumericBoundsAndClosedResults(self):
        params=dict(binding=binding(), context_refs=[ident("context")],max_bytes=1,max_tokens=1)
        for bad in [True,0,-1,4294967296]:
            with self.subTest(bad=bad), self.assertRaises(ValueError): decode(request("context", {**params,"max_bytes":bad}))
        ok=dict(protocol=P,request_id="r1",ok=True,result=dict(protocols=[P],backend_version="test",capabilities=[]))
        self.assertTrue(decode(ok).ok)
        for change in [{"error":{}},{"binding":None}]:
            with self.assertRaises(ValueError): decode({**ok,**change})
        fail=dict(protocol=P,request_id="r1",ok=False,error=dict(code="UNSUPPORTED",message="no",retryable=False,details=[]))
        self.assertFalse(decode(fail).ok)
        with self.assertRaises(ValueError): decode({**fail,"ok":0})

if __name__ == "__main__": unittest.main()
