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


"""Exercise published consumer schemas against real controller output.

The small validator below supports the schema keywords used by these contracts;
no runtime dependency on a third-party JSON-schema package is introduced.
"""
import json
from pathlib import Path
import unittest
import test_review_controller

ROOT=Path(__file__).resolve().parents[1]/"schemas"

def validate(value,schema,document):
    if "$ref" in schema:
        path,_,fragment=schema["$ref"].partition("#")
        if path:document=json.loads((ROOT/path).read_text())
        target=document
        for part in fragment.split("/")[1:]:target=target[part]
        return validate(value,target,document)
    if "oneOf" in schema:
        matches=0
        for branch in schema["oneOf"]:
            try:validate(value,branch,document);matches+=1
            except ValueError:pass
        if matches!=1:raise ValueError("oneOf")
    if "const" in schema and value!=schema["const"]:raise ValueError("const")
    if "enum" in schema and value not in schema["enum"]:raise ValueError("enum")
    types=dict(object=dict,array=list,string=str,integer=int,boolean=bool,null=type(None))
    kind=schema.get("type")
    if kind and type(value) is not types[kind]:raise ValueError("type")
    if kind=="object":
        if not set(schema.get("required",[]))<=set(value):raise ValueError("required")
        properties=schema.get("properties",{})
        if schema.get("additionalProperties") is False and set(value)-set(properties):raise ValueError("extra")
        for k,v in value.items():
            if k in properties:validate(v,properties[k],document)
    if kind=="array":
        if len(value)<schema.get("minItems",0) or len(value)>schema.get("maxItems",2**64):raise ValueError("array size")
        for item in value:validate(item,schema["items"],document)
    if kind=="integer" and not schema.get("minimum",-(2**63))<=value<=schema.get("maximum",2**64-1):raise ValueError("range")
    if kind=="string":
        import re
        if len(value)<schema.get("minLength",0) or ("pattern" in schema and not re.search(schema["pattern"],value)):raise ValueError("string")

class ControllerSchemasTest(unittest.TestCase):
    setUp=test_review_controller.ControllerTest.setUp
    response=test_review_controller.ControllerTest.response
    advance=test_review_controller.ControllerTest.advance

    def test_published_response_and_report_contracts(self):
        from test_candidates import proposal
        from veritas_review.reporting import render_report
        response=self.response();response["candidate_proposals"]=[proposal(kind="semantic_observation.v1")];response["finish"]=True
        schema=json.loads((ROOT/"veritas-model-response-v1.schema.json").read_text())
        validate(response,schema,schema)
        self.advance(response)
        report=render_report(self.state)
        schema=json.loads((ROOT/"veritas-review-report-v1.schema.json").read_text())
        validate(report,schema,schema)
        report["authority"]="MUST"
        with self.assertRaises(ValueError):validate(report,schema,schema)
