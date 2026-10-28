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


"""Frozen backend v2 wire models. No transport, controller or proof authority."""
from dataclasses import dataclass, make_dataclass
import hashlib
import json
from pathlib import Path
import re
from typing import ClassVar

PROTOCOL = "veritas.review.backend.v2"
_SCHEMA = json.loads((Path(__file__).resolve().parents[2] / "schemas" /
                      "veritas-review-backend-v2.schema.json").read_text(encoding="utf-8"))

@dataclass(frozen=True)
class ArtifactId:
    value: str
    kind: ClassVar[str] = ""
    def __post_init__(self):
        if not isinstance(self.value, str) or not re.fullmatch(re.escape(self.kind)+r":sha256:[0-9a-f]{64}", self.value):
            raise ValueError("wrong artifact kind or malformed ID")

for _name, _kind in [("SnapshotId","review-snapshot"),("ProgramScopeId","program-scope"),
    ("ContextId","context"),("CandidateId","candidate"),("LocalEvidenceId","local-evidence"),
    ("ObligationId","obligation"),("VerificationId","verification"),
    ("ReviewReportId","review-report"),("StructuralIndexId","structural-index")]:
    globals()[_name] = type(_name, (ArtifactId,), {"kind":_kind, "__module__":__name__})


def _semantic(value, depth=0):
    if depth > 128: raise ValueError("JSON nesting limit")
    if value is None or type(value) is bool: return
    if type(value) is int:
        if not -(2**63) <= value <= 2**64-1: raise ValueError("integer overflow")
        return
    if type(value) is str:
        value.encode("utf-8", "strict")
        return
    if type(value) in (tuple,list):
        for v in value: _semantic(v, depth+1)
        return
    if type(value) is dict:
        for k,v in value.items():
            if type(k) is not str: raise ValueError("non-string JSON key")
            _semantic(k, depth+1); _semantic(v, depth+1)
        return
    raise ValueError("semantic JSON supports bounded integers only")


def canonical_json(value):
    """Canonical UTF-8 without Unicode normalization or audit fields."""
    try:
        _semantic(value)
        return json.dumps(value,ensure_ascii=False,sort_keys=True,separators=(",", ":"),allow_nan=False).encode("utf-8")
    except (UnicodeError, RecursionError) as error:
        raise ValueError("invalid UTF-8 or nesting") from error


def artifact_id(kind, semantic, *, audit=None):
    """audit is deliberately separate from the semantic payload."""
    if not isinstance(kind,type) or not issubclass(kind,ArtifactId) or not kind.kind:
        raise ValueError("artifact type required")
    digest=hashlib.sha256(("veritas.review."+kind.kind+".v1\0").encode()+canonical_json(semantic)).hexdigest()
    return kind(kind.kind+":sha256:"+digest)


def _resolve(schema):
    return _SCHEMA["$defs"][schema["$ref"].split("/")[-1]] if "$ref" in schema else schema


def _validate(value, schema):
    schema=_resolve(schema)
    if "oneOf" in schema:
        matches=0
        for branch in schema["oneOf"]:
            try: _validate(value,branch); matches+=1
            except ValueError: pass
        if matches != 1: raise ValueError("value does not match exactly one closed schema")
        return
    if "const" in schema and (type(value) is not type(schema["const"]) or value != schema["const"]):
        raise ValueError("wrong protocol, operation or discriminant")
    if "enum" in schema and value not in schema["enum"]: raise ValueError("unknown enum")
    t=schema.get("type")
    types={"null":type(None),"boolean":bool,"integer":int,"string":str,"array":list,"object":dict}
    if t and type(value) is not types[t]: raise ValueError("wrong JSON type")
    if t=="object":
        if set(value) != set(schema["required"]): raise ValueError("unknown or missing key")
        for k,v in value.items(): _validate(v,schema["properties"][k])
    elif t=="array":
        if len(value)<schema.get("minItems",0): raise ValueError("empty array")
        if schema.get("uniqueItems") and len({canonical_json(v) for v in value}) != len(value): raise ValueError("duplicate set member")
        for v in value: _validate(v,schema["items"])
    elif t=="integer":
        if not schema.get("minimum", -(2**63)) <= value <= schema.get("maximum",2**64-1): raise ValueError("integer outside range")
    elif t=="string":
        if len(value)<schema.get("minLength",0): raise ValueError("empty string")
        if "pattern" in schema and not re.search(schema["pattern"],value): raise ValueError("invalid string")
    if t=="object": _invariants(value, schema.get("title"))


def _invariants(value, name):
    if name=="ReviewBinding":
        versions=value["provider_versions"]
        if versions != sorted(versions,key=lambda p:(p["name"],p["version"],p["config_digest"])): raise ValueError("provider versions must be sorted")
        if len({p["name"] for p in versions}) != len(versions): raise ValueError("duplicate provider")
    if name=="LocalRequest" and value["requested_slots"] != sorted(value["requested_slots"]): raise ValueError("slots must be sorted")
    if name in {"AnchorRef","Region","EntityBinding","ContextArtifact"} and value["start"]>value["end"]: raise ValueError("reversed span")
    if name=="ContextArtifact":
        if value["end"]-value["start"] != len(value["excerpt"].encode("utf-8")): raise ValueError("excerpt byte span mismatch")
        for item in value["entity_bindings"]:
            if item["start"]<value["start"] or item["end"]>value["end"]: raise ValueError("entity outside context")
    if name=="ProgramScope" and value["membership_source"]=="OUTPUT_HINT" and value["coverage"]=="DECLARED_CLOSED": raise ValueError("output hint cannot close membership")

class _WireModel:
    @classmethod
    def from_wire(cls, value):
        _semantic(value); _validate(value,{"$ref":"#/$defs/"+cls.__name__})
        return _freeze(value,{"$ref":"#/$defs/"+cls.__name__})

# Every schema object becomes a frozen dataclass; nested collections are tuples.
# ID-valued fields are converted to the distinct tagged classes above.
for _name,_definition in _SCHEMA["$defs"].items():
    if _definition.get("type")=="object":
        globals()[_name]=make_dataclass(_name,[(field,object) for field in _definition["required"]],bases=(_WireModel,),frozen=True,namespace={"__module__":__name__})


def _freeze(value, schema):
    if "$ref" in schema:
        name=schema["$ref"].split("/")[-1]
        if name.endswith("Id"): return globals()[name](value)
    schema=_resolve(schema)
    if "oneOf" in schema:
        for branch in schema["oneOf"]:
            try: _validate(value,branch)
            except ValueError: continue
            return _freeze(value,branch)
    if schema.get("type")=="object":
        return globals()[schema["title"]](**{k:_freeze(v,schema["properties"][k]) for k,v in value.items()})
    if schema.get("type")=="array":return tuple(_freeze(v,schema["items"]) for v in value)
    return value

@dataclass(frozen=True)
class BackendEnvelope:
    protocol: str
    request_id: str
    operation: str | None = None
    parameters: object = None
    ok: bool | None = None
    result: object = None
    error: object = None


def decode_v2(data: bytes, *, expected_request_id=None, expected_operation=None) -> BackendEnvelope:
    """Reject malformed/oversized wire data before returning immutable models."""
    if type(data) is not bytes or len(data)>1024*1024: raise ValueError("bounded bytes required")
    def pairs(entries):
        result={}
        for k,v in entries:
            if k in result: raise ValueError("duplicate JSON key")
            result[k]=v
        return result
    def forbidden(_): raise ValueError("non-integer JSON number")
    try:
        value=json.loads(data.decode("utf-8","strict"),object_pairs_hook=pairs,parse_float=forbidden,parse_constant=forbidden)
        _semantic(value); _validate(value,_SCHEMA)
    except (UnicodeError,RecursionError) as error:
        raise ValueError("invalid UTF-8 or nesting") from error
    if expected_request_id is not None and value["request_id"]!=expected_request_id: raise ValueError("request ID mismatch")
    if expected_operation is not None:
        names={"handshake":"HandshakeResult","capture":"CaptureResult","context":"ContextResult","seed-candidates":"SeedResult","local-evidence":"LocalEvidence","native-evidence":"NativeResult","release":"ReleaseResult"}
        if "operation" in value and value["operation"] != expected_operation: raise ValueError("operation mismatch")
        if value.get("ok") is True:
            if expected_operation not in names: raise ValueError("unsupported operation")
            _validate(value["result"],ref_schema:={"$ref":"#/$defs/"+names[expected_operation]})
    for branch in _SCHEMA["oneOf"]:
        try: _validate(value,branch)
        except ValueError: continue
        frozen=_freeze(value,branch)
        return BackendEnvelope(**{k:getattr(frozen,k) for k in _resolve(branch)["required"]})
    raise ValueError("invalid envelope")
