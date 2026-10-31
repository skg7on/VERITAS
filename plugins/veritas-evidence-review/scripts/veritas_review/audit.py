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


"""Hash-chained WAL and atomic checkpoint in one private operator directory.

The WAL is authoritative across a crash between fsync(event) and checkpoint
rename. In-flight requests recover as ABORTED; they are never redispatched.
"""
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import secrets
from . import review_types as t


def _read(path):
    fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW)
    with os.fdopen(fd,"rb") as f:return f.read(32*1024*1024+1)


def _atomic(path,raw):
    temp=path.with_name(".tmp-"+secrets.token_hex(16))
    fd=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    try:
        with os.fdopen(fd,"wb") as f:f.write(raw);f.flush();os.fsync(f.fileno())
        os.replace(temp,path)
        parent=os.open(path.parent,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
        try:os.fsync(parent)
        finally:os.close(parent)
    finally:
        if temp.exists():temp.unlink()

class AuditStore:
    def __init__(self,root):
        self.root=Path(root).absolute()
        if self.root.is_symlink():raise ValueError("symlink state directory")
        self.root=self.root.parent.resolve()/self.root.name
        self.root.mkdir(mode=0o700,parents=True,exist_ok=True)
        if self.root.stat().st_mode & 0o077:raise ValueError("state directory must be private")

    @contextmanager
    def locked(self):
        fd=os.open(self.root/"lock",os.O_RDWR|os.O_CREAT|os.O_NOFOLLOW,0o600)
        try:
            fcntl.flock(fd,fcntl.LOCK_EX)
            yield
        finally:os.close(fd)

    def key(self):
        path=self.root/"key"
        try:
            fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
        except FileExistsError:return _read(path)
        with os.fdopen(fd,"wb") as f:f.write(secrets.token_bytes(32));f.flush();os.fsync(f.fileno())
        return _read(path)

    def save(self,data,event):
        path=self.root/"events.jsonl"
        previous=data.get("audit_head","0"*64);sequence=data.get("audit_sequence",0)+1
        state={k:v for k,v in data.items() if k not in {"audit_head","audit_sequence"}}
        entry=dict(sequence=sequence,previous=previous,event=event,state=state)
        entry["hash"]=t.hash_bytes(t.canonical_json(entry))
        raw=t.canonical_json(entry)+b"\n"
        if (path.stat().st_size if path.exists() else 0)+len(raw)>data["policy"]["audit_bytes"]-1024:
            if path.exists() and (self.root/"checkpoint.json").exists():
                self.abort("audit byte budget exhausted",costs=data["costs"])
            raise ValueError("audit byte budget")
        fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_APPEND|os.O_NOFOLLOW,0o600)
        with os.fdopen(fd,"ab") as f:f.write(raw);f.flush();os.fsync(f.fileno())
        data["audit_head"]=entry["hash"];data["audit_sequence"]=sequence
        _atomic(self.root/"checkpoint.json",t.canonical_json(dict(sequence=sequence,head=entry["hash"],state=state)))

    def abort(self,reason,*,costs=None):
        """Use the reserved 1 KiB terminal WAL slot, without copying full state.

        Caller holds the attempt lock. The previous durable reservation is kept
        even if the worker result could not be published within the audit cap.
        """
        data=self.load()
        if data["status"]=="ABORTED":return
        reason=str(reason)[:128]
        entry=dict(sequence=data["audit_sequence"]+1,previous=data["audit_head"],event="aborted",state=None,reason=reason,costs=costs or {})
        entry["hash"]=t.hash_bytes(t.canonical_json(entry))
        raw=t.canonical_json(entry)+b"\n"
        path=self.root/"events.jsonl"
        if len(raw)>1024 or path.stat().st_size+len(raw)>data["policy"]["audit_bytes"]:raise ValueError("terminal audit byte budget")
        fd=os.open(path,os.O_WRONLY|os.O_APPEND|os.O_NOFOLLOW)
        with os.fdopen(fd,"ab") as f:f.write(raw);f.flush();os.fsync(f.fileno())
        state={k:v for k,v in data.items() if k not in {"audit_head","audit_sequence"}}
        _abort_state(state,reason,costs or {})
        _atomic(self.root/"checkpoint.json",t.canonical_json(dict(sequence=entry["sequence"],head=entry["hash"],state=state)))

    def load(self):
        try:
            raw=_read(self.root/"events.jsonl")
            checkpoint=json.loads(_read(self.root/"checkpoint.json"))
            if not raw.endswith(b"\n"):raise ValueError("incomplete event chain")
            previous="0"*64;latest=None;checkpoint_seen=False
            for seq,line in enumerate(raw.splitlines(),1):
                entry=json.loads(line);claimed=entry.pop("hash")
                if entry["sequence"]!=seq or entry["previous"]!=previous or t.hash_bytes(t.canonical_json(entry))!=claimed:raise ValueError("event chain mismatch")
                previous=claimed
                if entry["event"]=="aborted":
                    if latest is None or entry["state"] is not None:raise ValueError("invalid abort event")
                    _abort_state(latest,entry["reason"],entry["costs"])
                else:latest=entry["state"]
                if seq==checkpoint["sequence"]:
                    checkpoint_seen=checkpoint["head"]==claimed and checkpoint["state"]==latest
            if not checkpoint_seen or latest is None:raise ValueError("checkpoint mismatch")
            return {**latest,"audit_sequence":seq,"audit_head":previous}
        except (OSError,KeyError,TypeError,json.JSONDecodeError) as e:raise ValueError("corrupt review audit/checkpoint") from e


def _abort_state(state,reason,costs):
    state["status"]="ABORTED";state["grants"]=[]
    for key,value in costs.items():state["costs"][key]=max(state["costs"].get(key,0),value)
    state["unknowns"].append("Interrupted or refused dispatch: "+reason+"; durable reservation retained; no automatic redispatch.")
