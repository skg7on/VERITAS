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


"""Host-neutral controller policy and handles. Trusted host metadata is separate."""
from dataclasses import asdict, dataclass, replace
from pathlib import Path
from . import review_types as t

@dataclass(frozen=True)
class ReviewPolicy:
    max_turns: int = 8
    max_candidates: int = 16
    input_tokens: int = 32000
    response_bytes: int = 65536
    context_bytes: int = 262144
    turn_context_bytes: int = 32768
    turn_context_tokens: int = 8000
    output_tokens: int = 4096
    backend_wall_ms: int = 120000
    backend_call_ms: int = 60000
    backend_output_bytes: int = 8*1024*1024
    backend_examined: int = 1000000
    audit_bytes: int = 16*1024*1024
    report_bytes: int = 2*1024*1024

    def __post_init__(self):
        if any(type(v) is not int or v <= 0 for v in asdict(self).values()):
            raise ValueError("positive integer review limits required")
        ceilings=dict(max_turns=8,max_candidates=16,response_bytes=65536,output_tokens=4096,
                      turn_context_bytes=32768,turn_context_tokens=8000,backend_call_ms=60000)
        if any(getattr(self,k)>v for k,v in ceilings.items()):
            raise ValueError("review protocol ceiling exceeded")

    def lower(self, **limits):
        if any(k not in asdict(self) or type(v) is not int or v<=0 or v>getattr(self,k) for k,v in limits.items()):
            raise ValueError("model can only lower limits")
        return replace(self, **limits)

@dataclass(frozen=True)
class ReviewOptions:
    state_dir: Path
    project_key: str
    base_ref: str
    target_ref: str | None = None
    selected_untracked: tuple[str, ...] = ()
    program_manifest_key: str | None = None
    context_selection_keys: tuple[str, ...] = ()

    def capture_parameters(self):
        return dict(project_key=self.project_key,base_ref=self.base_ref,target_ref=self.target_ref,
                    selected_untracked=list(self.selected_untracked),program_manifest_key=self.program_manifest_key,
                    context_selection_keys=list(self.context_selection_keys))

@dataclass(frozen=True)
class ReviewState:
    state_dir: Path

@dataclass(frozen=True)
class Turn:
    state: ReviewState
    payload: bytes
    capability: str | None

    def as_dict(self):
        import json
        return {**json.loads(self.payload),"capability":self.capability}
    @property
    def binding(self):return self.as_dict()["binding"]
    @property
    def status(self):return self.as_dict()["status"]
    @property
    def contexts(self):return self.as_dict()["contexts"]
    @property
    def requests(self):return self.as_dict()["requests"]
