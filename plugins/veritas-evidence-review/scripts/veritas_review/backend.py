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


"""Bounded resident subprocess transport; trusted argv, no practice fallback."""
from dataclasses import dataclass
import os
from pathlib import Path
import selectors
import signal
import subprocess
import tempfile
import threading
import time
from . import review_types as t

_OPERATIONS=frozenset({"handshake","capture","context","seed-candidates","local-evidence","native-evidence","verify","release"})

class BackendError(RuntimeError):
    def __init__(self, message, *, fatal=True):
        super().__init__(message);self.fatal=fatal

@dataclass(frozen=True)
class ProcessLimits:
    wall_ms: int = 60000
    aggregate_wall_ms: int = 120000
    output_bytes: int = 1024*1024
    stderr_bytes: int = 64*1024
    request_bytes: int = 1024*1024
    rss_bytes: int = 1024*1024*1024
    aggregate_rss_bytes: int = 2*1024*1024*1024
    sample_ms: int = 250
    max_processes: int = 32
    hard_isolation: bool = False

    def __post_init__(self):
        for key,value in self.__dict__.items():
            if key=="hard_isolation":
                if type(value) is not bool: raise ValueError("boolean isolation policy required")
            elif type(value) is not int or value<=0: raise ValueError("positive integer process limits required")
        if self.sample_ms>250: raise ValueError("RSS sample interval must be at most 250 ms")
        if max(self.output_bytes,self.request_bytes)>1024*1024: raise ValueError("wire limit is one MiB")

@dataclass(frozen=True)
class BackendResult:
    envelope: t.BackendEnvelope
    transport_usage: object
    @property
    def result(self): return self.envelope.result

class Backend:
    def __init__(self, argv, *, cwd, limits=ProcessLimits()):
        if (type(argv) is not tuple or not argv or
            any(type(x) is not str or not x or "\0" in x for x in argv) or
            not os.path.isabs(argv[0])):
            raise ValueError("trusted absolute executable argv tuple required")
        if not Path(cwd).is_absolute(): raise ValueError("absolute configured working directory required")
        self.argv=argv;self.cwd=cwd;self.limits=limits
        self._process=None;self._private=None;self._counter=0;self._closed=False
        self._capabilities={};self._binding=None;self._wall_ms=0;self._lock=threading.Lock()
        self.last_usage=None
        self._session_groups=set()

    @property
    def pid(self):return self._process.pid if self._process is not None else None

    def handshake(self):return self.call("handshake",{"accepted_protocols":[t.PROTOCOL]})

    def _start(self):
        if self.limits.hard_isolation:
            raise BackendError("UNSUPPORTED: no enforcing hard isolation profile configured")
        self._private=tempfile.TemporaryDirectory(prefix="veritas-review-",dir=self.cwd)
        env={"PATH":"/usr/bin:/bin","HOME":self._private.name,"TMPDIR":self._private.name,
             "LANG":"C.UTF-8","LC_ALL":"C.UTF-8","SOURCE_DATE_EPOCH":"0"}
        try:
            self._process=subprocess.Popen(self.argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                 cwd=self._private.name,env=env,start_new_session=True,close_fds=True,bufsize=0)
            for stream in (self._process.stdin,self._process.stdout,self._process.stderr):os.set_blocking(stream.fileno(),False)
        except OSError as error:
            self.close();raise BackendError("backend launch failed") from error

    def _process_table(self):
        # Bound the OS sampler itself, including its stdout and deadline.
        process=subprocess.Popen(("/bin/ps","-A","-o","pid=,ppid=,pgid=,rss="),
            stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,close_fds=True,
            start_new_session=True,env={"PATH":"/usr/bin:/bin","LC_ALL":"C"})
        data=bytearray();deadline=time.monotonic()+0.5
        try:
            os.set_blocking(process.stdout.fileno(),False)
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout,selectors.EVENT_READ)
                while True:
                    remaining=deadline-time.monotonic()
                    if remaining<=0:raise BackendError("RSS sampler deadline exceeded")
                    if not selector.select(remaining):continue
                    chunk=os.read(process.stdout.fileno(),65536)
                    if not chunk:break
                    data.extend(chunk)
                    if len(data)>1024*1024:raise BackendError("RSS sampler output limit")
            if process.wait(timeout=max(0.001,deadline-time.monotonic()))!=0:
                raise BackendError("RSS sampler failed")
            rows=[]
            for line in data.splitlines():
                pid,parent,group,rss=line.split()
                rows.append((int(pid),int(parent),int(group),int(rss)*1024))
            return rows
        except (OSError,ValueError,subprocess.SubprocessError) as error:
            raise BackendError("RSS sampling unavailable") from error
        finally:
            if process.poll() is None:process.kill()
            process.wait(timeout=1)
            process.stdout.close()

    def _owned_processes(self, rows):
        # POSIX getsid returns the numeric leader ID. Do not use ps's `sess`
        # column: macOS can redact it to zero for unrelated sessions alike.
        members=[]
        for row in rows:
            try:session=os.getsid(row[0])
            except (ProcessLookupError,PermissionError):continue
            if session==self.pid:members.append(row)
        self._session_groups={row[2] for row in members}
        return members

    def _sample(self):
        rows=self._process_table();members=self._owned_processes(rows)
        own=next((row[3] for row in rows if row[0]==os.getpid()),0)
        return max((row[3] for row in members),default=0),sum(row[3] for row in members)+own,len(members)

    def call(self, operation, parameters, binding=None):
        with self._lock:
            if operation not in _OPERATIONS:raise ValueError("invalid operation")
            if self._closed:raise BackendError("backend session closed")
            if operation!="handshake" and operation not in self._capabilities:
                raise BackendError("UNSUPPORTED: operation was not advertised")
            if isinstance(parameters,t._WireModel):parameters=t._wire(parameters)
            if type(parameters) is not dict:raise ValueError("closed operation parameters required")
            if binding is not None:
                supplied=t._wire(binding) if isinstance(binding,t._WireModel) else binding
                if parameters.get("binding") != supplied:raise ValueError("parameter binding mismatch")
            if operation not in {"handshake","capture"} and parameters.get("binding") != self._binding:
                raise ValueError("request does not match advertised snapshot")
            self._counter+=1;request_id="request-"+str(self._counter)
            request=t.canonical_json({"protocol":t.PROTOCOL,"request_id":request_id,"operation":operation,"parameters":parameters})
            t.decode_v2(request)
            if len(request)>self.limits.request_bytes:raise ValueError("request byte limit")
            remaining=self.limits.aggregate_wall_ms-self._wall_ms
            if remaining<=0:raise BackendError("aggregate backend deadline exhausted")
            start=time.monotonic();budget=min(self.limits.wall_ms,remaining)
            peak=0;output=bytearray();stderr_count=0;samples=0;sent=0
            try:
                if self._process is None:self._start()
                process=self._process;payload=request+b"\n"
                deadline=start+budget/1000;next_sample=start
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stdout,selectors.EVENT_READ,"stdout")
                    selector.register(process.stderr,selectors.EVENT_READ,"stderr")
                    selector.register(process.stdin,selectors.EVENT_WRITE,"stdin")
                    while True:
                        now=time.monotonic()
                        if now>=deadline:raise BackendError("backend deadline exceeded")
                        if now>=next_sample:
                            worker,total,count=self._sample();samples+=1;peak=max(peak,worker)
                            if worker>self.limits.rss_bytes or total>self.limits.aggregate_rss_bytes or count>self.limits.max_processes:
                                raise BackendError("observed process/RSS allowance exhausted")
                            next_sample=time.monotonic()+self.limits.sample_ms/1000
                        events=selector.select(max(0,min(deadline,next_sample)-time.monotonic()))
                        complete=False
                        for key,_ in events:
                            if key.data=="stdin":
                                try:sent+=os.write(key.fd,payload[sent:])
                                except BlockingIOError:continue
                                if sent==len(payload):selector.unregister(key.fileobj)
                                continue
                            try:chunk=os.read(key.fd,65536)
                            except BlockingIOError:continue
                            if not chunk:
                                selector.unregister(key.fileobj)
                                if key.data=="stdout":raise BackendError("backend closed stdout before response")
                                continue
                            if key.data=="stderr":
                                stderr_count+=len(chunk)
                                if stderr_count>self.limits.stderr_bytes:raise BackendError("backend stderr limit")
                            else:
                                output.extend(chunk)
                                if len(output)>self.limits.output_bytes:raise BackendError("backend stdout limit")
                                if b"\n" in output:
                                    line,extra=bytes(output).split(b"\n",1)
                                    if extra:raise BackendError("multiple/trailing backend responses")
                                    envelope=t.decode_v2(line,expected_request_id=request_id,expected_operation=operation)
                                    complete=True
                        if complete:break
                if type(envelope.ok) is not bool:
                    raise BackendError("expected backend response envelope")
                if envelope.ok is not True:
                    raise BackendError(envelope.error.code+": "+envelope.error.message, fatal=False)
                result=t._wire(envelope.result)
                if operation=="handshake":
                    if t.PROTOCOL not in result["protocols"]:raise BackendError("v2 negotiation failed")
                elif operation=="capture":
                    if result["binding"]["native_binding"] is not None:raise BackendError("unexpected native capture binding")
                    self._binding=result["binding"]
                elif result.get("binding") != self._binding:
                    raise BackendError("backend returned a different snapshot binding")
                if operation in {"handshake","capture"}:
                    caps=result["capabilities"]
                    if len({c["operation"] for c in caps})!=len(caps) or any(c["operation"] not in _OPERATIONS for c in caps):
                        raise BackendError("invalid advertised capability")
                    self._capabilities={c["operation"]:c for c in caps}
                elif operation=="release" and result["released"]:
                    self._binding=None
                    self._capabilities={op:cap for op,cap in self._capabilities.items() if op=="capture"}
            except (OSError,ValueError,BackendError) as error:
                if not isinstance(error,BackendError) or error.fatal:self.close()
                if isinstance(error,BackendError):raise
                raise BackendError("invalid backend transport response") from error
            finally:
                elapsed=int((time.monotonic()-start)*1000+0.999)
                self._wall_ms+=elapsed
                self.last_usage={"wall_ms":elapsed,"peak_rss_bytes":peak,"output_bytes":len(output),
                     "stderr_bytes":stderr_count,"rss_samples":samples,
                     "deadline_overshoot_ms":max(0,elapsed-budget),"rss_sample_ms":self.limits.sample_ms}
            return BackendResult(envelope,self.last_usage.copy())

    def close(self):
        process=self._process;self._closed=True
        if process is not None:
            # Sample before killing the supervisor so children cannot disappear
            # from the parent tree before their separate groups are identified.
            try:self._owned_processes(self._process_table())
            except (OSError,BackendError):pass
            groups=set(self._session_groups)
            if process.poll() is None:groups.add(process.pid)
            for group in groups:
                if group<=1 or group==os.getpgrp():continue
                try:os.killpg(group,signal.SIGKILL)
                except ProcessLookupError:pass
            try:process.wait(timeout=2)
            finally:
                for stream in (process.stdin,process.stdout,process.stderr):stream.close()
            self._process=None;self._session_groups.clear()
        if self._private is not None:self._private.cleanup();self._private=None
    def __enter__(self):return self
    def __exit__(self,*args):self.close()
