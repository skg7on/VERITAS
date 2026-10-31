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


"""Validate only advertised pinned UTF-8 byte anchors."""
from . import review_types as t


def validate_anchor(anchor, state):
    t.AnchorRef.from_wire(anchor)
    context=state["contexts"].get(anchor["context_id"])
    if context is None or context["binding"]!=state["binding"]:raise ValueError("unadvertised or stale context")
    if not context["start"]<=anchor["start"]<anchor["end"]<=context["end"]:raise ValueError("anchor outside context")
    data=context["excerpt"].encode("utf-8")
    try:
        data[:anchor["start"]-context["start"]].decode("utf-8")
        data[:anchor["end"]-context["start"]].decode("utf-8")
    except UnicodeError as e:raise ValueError("anchor splits UTF-8") from e
    matching=[e for e in context["entity_bindings"] if e["start"]==anchor["start"] and e["end"]==anchor["end"]]
    if anchor["entity_ref"] is not None:
        if not any(e["entity_ref"]==anchor["entity_ref"] for e in matching):raise ValueError("invented entity reference")
    return matching


def validate_context(artifact, state):
    t.ContextArtifact.from_wire(artifact)
    if artifact["binding"]!=state["binding"] or artifact["context_id"] not in state["context_refs"]:
        raise ValueError("unadvertised context or stale binding")
    old=state["contexts"].get(artifact["context_id"])
    if old is not None and old!=artifact:raise ValueError("immutable context changed")
