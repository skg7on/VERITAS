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

"""Render an evidence-backed decision without filling pending metrics with zeros."""
import argparse
import json
from pathlib import Path
try:
    from .evaluate import evaluate
except ImportError:
    from evaluate import evaluate


def markdown(result):
    lines=["# RDX11 review qualification", "", "Qualification: **"+result["qualification"]+"**", "",
           result["authority"],"", "| Profile | Availability | Admitted attempts | Discovery precision | Discovery recall |", "| --- | --- | ---: | ---: | ---: |"]
    for name,profile in result["profiles"].items():
        q=profile.get("quality",{}).get("whole_discovery",{})
        def value(key):return "pending" if q.get(key) is None else str(q[key])
        lines.append(f"| {name} | {profile['availability']} | {value('attempts')} | {value('precision')} | {value('recall')} |")
    lines.extend(["", "P95 requires >=100 attempts per workload/run; full measurements require >=3 fresh runs.",
                  "Machine-readable JSON retains separate costs, memory/bytes, tokens, misses and per-run variability.", "",result["p4_investment"],"",result["scaling"],"", "Outstanding evidence:", ""])
    lines.extend("- "+gap for gap in result["gaps"])
    return "\n".join(lines)+"\n"


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus",type=Path);parser.add_argument("run",type=Path)
    parser.add_argument("--judgments",type=Path);parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    from validate_corpus import load_corpus,validate_corpus
    corpus=load_corpus(args.corpus);validate_corpus(corpus,corpus_path=args.corpus)
    result=evaluate(corpus,json.loads(args.run.read_text()),json.loads(args.judgments.read_text()) if args.judgments else [])
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.with_suffix(".json").write_text(json.dumps(result,sort_keys=True,indent=2,allow_nan=False)+"\n")
    args.output.with_suffix(".md").write_text(markdown(result))
    return 2  # The production release validator is not implemented yet.


if __name__=="__main__":raise SystemExit(main())
