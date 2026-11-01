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

"""Paired operator-adapter execution. No model/provider is selected implicitly."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import time


def digest(value):
    return "sha256:" + hashlib.sha256(json.dumps(value,sort_keys=True,separators=(",",":"),allow_nan=False).encode()).hexdigest()


def pending(corpus):
    return {"version":"veritas.review.eval.run.v1","release_digest":corpus["release_digest"],
            "configuration":None,"attempts":[],"reason":"authenticated model transport, production profile adapters and independent measurements pending"}


def schedule(corpus, *, repeats, runs):
    if type(repeats) is not int or repeats < 1 or type(runs) is not int or runs < 1:
        raise ValueError("positive integer repeats and runs required")
    return [dict(profile=profile,mode=mode,case_id=case,run_id=run,sample_id=sample)
            for run in range(runs) for sample in range(repeats)
            for mode, mode_policy in corpus["runs"].items()
            for case in mode_policy["case_ids"] for profile, policy in corpus["budgets"]["profiles"].items()
            if policy["availability"]=="AVAILABLE"]


def request_for(case, job, context, corpus):
    # Case names, oracle, split, expected outcome and rubric never reach the model.
    request={"version":"veritas.review.eval.request.v1","profile":job["profile"],
             "mode":job["mode"],"base_revision":case["base_revision"],
             "head_revision":case["head_revision"],"paths":case["change"]["paths"],
             "attempt_nonce":digest(job),"context":context,"budget":corpus["budgets"]["profiles"][job["profile"]]}
    if job["mode"]=="fixed_candidate":
        oracle=case["oracle"]
        request["candidate"]={"predicate":oracle["predicate"]}
        if "callsite" in oracle:
            request["candidate"]["callsite"]=oracle["callsite"]
    return request


def _rss(pid, groups):
    """Sample the runner and adapter session, including separate compiler groups.

    Trusted adapters must not daemonize into new sessions. This sampler is not
    an enforcing containment boundary and cannot promise to catch fast orphans.
    """
    try:
        rows=subprocess.check_output(["/bin/ps","-axo","pid=,ppid=,pgid=,rss="],timeout=.5,text=True,stderr=subprocess.DEVNULL)
        table=[tuple(map(int,line.split())) for line in rows.splitlines() if len(line.split())==4]
        owned={pid}
        while True:
            children={p for p,parent,_,_ in table if parent in owned}
            if children <= owned: break
            owned |= children
        for p,_,_,_ in table:
            try:
                if os.getsid(p)==pid:owned.add(p)
            except (ProcessLookupError,PermissionError):pass
        groups.update(group for p,_,group,_ in table if p in owned)
        return (sum(rss*1024 for p,_,_,rss in table if p in owned or p==os.getpid()),len(owned))
    except (OSError,ValueError,subprocess.SubprocessError):
        return None, None


def execute(argv, request, directory, *, wall_ms, output_bytes):
    if not isinstance(argv,list) or not argv or not os.path.isabs(argv[0]) or not all(isinstance(x,str) and x and "\0" not in x for x in argv):
        raise ValueError("adapter argv must be a nonempty string array")
    if wall_ms <= 0 or output_bytes <= 0: raise ValueError("positive execution limits required")
    if directory.exists() and any(directory.iterdir()):
        raise ValueError("attempt artifact directory must be empty")
    directory.mkdir(parents=True,exist_ok=True)
    request_data=json.dumps(request,sort_keys=True,allow_nan=False).encode()
    (directory/"request.json").write_bytes(request_data)
    start=time.monotonic(); streams={}; output={"stdout":bytearray(),"stderr":bytearray()}
    status="EXECUTED"; peak=None; processes=None; process=None; groups=set()
    try:
        with (directory/"request.json").open("rb") as stdin, selectors.DefaultSelector() as selector:
            process=subprocess.Popen(argv,stdin=stdin,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                     start_new_session=True,cwd=directory)
            for name, stream in (("stdout",process.stdout),("stderr",process.stderr)):
                streams[name]=stream
                os.set_blocking(stream.fileno(),False);selector.register(stream,selectors.EVENT_READ,name)
            while selector.get_map() or process.poll() is None:
                if (time.monotonic()-start)*1000 >= wall_ms:
                    status="TIMEOUT";break
                rss,count=_rss(process.pid,groups)
                if rss is not None: peak=max(peak or 0,rss);processes=max(processes or 0,count)
                for key,_ in selector.select(timeout=0.02):
                    data=os.read(key.fd,min(65536,output_bytes+1))
                    if not data: selector.unregister(key.fileobj);continue
                    output[key.data].extend(data)
                    if sum(map(len,output.values())) > output_bytes:
                        status="OUTPUT_LIMIT";break
                if status!="EXECUTED":break
            if status=="EXECUTED" and process.wait()!=0:status="ERROR"
    except OSError as error:
        status="ERROR";output["stderr"].extend(str(error).encode()[:output_bytes])
    finally:
        if process is not None:
            live_groups=set()
            _rss(process.pid,live_groups)
            if process.poll() is None:live_groups.add(process.pid)
            for group in live_groups:
                if group<=1 or group==os.getpgrp():continue
                try:os.killpg(group,signal.SIGKILL)
                except ProcessLookupError:pass
            process.wait()
        for stream in streams.values():stream.close()
    # Bound persisted output as well as live reads.
    stdout=bytes(output["stdout"][:output_bytes]);stderr=bytes(output["stderr"][:max(0,output_bytes-len(stdout))])
    (directory/"response.json").write_bytes(stdout);(directory/"stderr.txt").write_bytes(stderr)
    if status=="EXECUTED":
        try:json.loads(stdout)
        except (ValueError,UnicodeDecodeError):status="INVALID_RESPONSE"
    return {"status":status,"request_digest":digest(request),
            "response_digest":"sha256:"+hashlib.sha256(stdout).hexdigest(),
            "runner_wall_ms":(time.monotonic()-start)*1000,"aggregate_peak_rss_bytes":peak,
            "peak_processes":processes,"containment":"UNENFORCED: trusted adapters must not daemonize","memory_method":"sampled simultaneous runner+adapter descendants RSS; 20ms loop, short-lived peaks may be missed",
            "artifact_directory":str(directory.resolve())}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus",type=Path);parser.add_argument("--config",type=Path)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--repeats",type=int,default=100);parser.add_argument("--runs",type=int,default=3)
    args=parser.parse_args()
    from validate_corpus import load_corpus,validate_corpus
    corpus=load_corpus(args.corpus);validate_corpus(corpus,corpus_path=args.corpus)
    bundle=pending(corpus)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    if args.output.exists():raise ValueError("refusing to overwrite a run bundle")
    if args.config:
        config=json.loads(args.config.read_text())
        required=("model","provider","source_disclosure_policy","prompt_policy_digest","context_policy_digest","compiler","build_type","target","host","adapter_revision")
        if any(not config.get(key) for key in required):raise ValueError("operator configuration missing required comparison identity")
        bundle["configuration"]={key:config[key] for key in required}
        bundle["reason"]=None
        cases={case["case_id"]:case for family in corpus["families"] for case in family["cases"]}
        for job in schedule(corpus,repeats=args.repeats,runs=args.runs):
            key=f"{job['run_id']}/{job['sample_id']}/{job['mode']}/{job['case_id']}/{job['profile']}"
            # Operator prepares bounded profile-specific contexts from captured inputs.
            # A: source only; B: structural; C: local compiler evidence. The adapter
            # must execute the shared controller and publish its receipts in response.
            context_file=Path(config["contexts"][job["case_id"]][job["profile"]]).resolve()
            context=json.loads(context_file.read_text())
            request=request_for(cases[job["case_id"]],job,context,corpus)
            request["configuration"]=bundle["configuration"]
            budget=corpus["budgets"]["profiles"][job["profile"]]
            request_size=len(json.dumps(request).encode())
            if request_size>budget["context_bytes"]:raise ValueError("context exceeds frozen budget")
            result=execute(config["adapters"][job["profile"]],request,args.output.resolve().parent/"attempts"/key,
                           wall_ms=budget["wall_ms"],output_bytes=budget["response_bytes"])
            bundle["attempts"].append(dict(job,attempt_id=key,configuration_digest=digest(bundle["configuration"]),**result))
            # Durable partial record survives interruption, never a fabricated complete run.
            args.output.write_text(json.dumps(bundle,indent=2,allow_nan=False)+"\n")
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(bundle,indent=2,allow_nan=False)+"\n")


if __name__=="__main__":main()
