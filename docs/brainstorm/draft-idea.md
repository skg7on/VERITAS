I want to build a new static analysis system which combine the traditional static analysis engineering with LLMs driven semantic analysis code review.

I think the right long-term direction is **not “Static Analyzer + LLM Code Reviewer” as two independent tools**, but a **neuro-symbolic program analysis system** in which the two continuously constrain each other.

There is one nuance I would change in your premise: static analysis does understand *formal program semantics*—types, control flow, aliasing, data flow, value ranges, path constraints, etc. What it usually lacks is **intent-level semantics**: API contracts that were never modeled, business invariants, protocol/state-machine intent, architectural assumptions, whether a suspicious data flow is actually safe in the application context, and so on. LLMs are almost the inverse: strong at inferring intent and contextual relationships, but weak at exhaustive, deterministic reasoning over large state spaces. Recent neuro-symbolic work is increasingly exploiting exactly this complementarity. ([arXiv][1])

## 1. I would build the system around an “Analysis Evidence IR”

The key architectural decision is:

> **Do not feed an entire repository plus static-analysis warnings to an LLM.
> Make static analysis produce a structured, minimal, machine-verifiable evidence package, and let the Agent reason on that evidence.**

Conceptually:

```text
                     ┌─────────────────────────┐
                     │ Source / PR / Build DB  │
                     └────────────┬────────────┘
                                  │
                     ┌────────────▼────────────┐
                     │ Program Analysis Layer │
                     │                        │
                     │ AST / SSA / CFG        │
                     │ Call Graph / Points-to │
                     │ DFG / VFG / PDG / CPG  │
                     │ Abstract Interpretation│
                     │ Taint / Symbolic Exec  │
                     └────────────┬────────────┘
                                  │
                     Analysis Evidence IR
                                  │
          ┌───────────────────────▼────────────────────────┐
          │                                               │
          │ location / path / slice / constraints         │
          │ source-sink / alias / reaching-defs           │
          │ function summaries / call chain               │
          │ assumptions / sanitizer / ownership           │
          │ confidence / provenance                       │
          │                                               │
          └───────────────────────┬────────────────────────┘
                                  │
                     ┌────────────▼─────────────┐
                     │ Semantic Review Agent   │
                     │                         │
                     │ infer intent/contracts  │
                     │ classify findings       │
                     │ discover missing rules  │
                     │ generate hypotheses     │
                     │ explain defects         │
                     │ propose fixes           │
                     └────────────┬─────────────┘
                                  │
                           Proof obligations
                                  │
                  ┌───────────────▼────────────────┐
                  │ Deterministic Verification    │
                  │                               │
                  │ Static Analysis / SMT         │
                  │ Symbolic Execution            │
                  │ Compilation / Tests / Fuzzing │
                  └───────────────┬────────────────┘
                                  │
                  ┌───────────────▼────────────────┐
                  │ Verified Review Result        │
                  │                               │
                  │ Definite / Likely / Unknown   │
                  │ + evidence + explanation      │
                  └────────────────────────────────┘
```

I would call the intermediate representation something like **Analysis Evidence IR (AEIR)**.

That IR is, in my view, more important than the LLM itself.

---

# 2. The most important interaction should be bidirectional

Many first-generation implementations look like:

```text
Static Analyzer
      ↓
10,000 warnings
      ↓
LLM
      ↓
rank/filter warnings
```

This helps, but it only uses perhaps 20% of the potential.

The stronger architecture is:

```text
                 ┌───────────────┐
                 │ Static Engine │
                 └──────┬────────┘
                        │ evidence
                        ▼
                ┌───────────────┐
                │ Review Agent  │
                └──────┬────────┘
                       │
       semantic hypothesis / missing model
       new source/sink/sanitizer
       API contract / invariant
       suspicious call edge
       interesting path
                       │
                       ▼
                ┌───────────────┐
                │ Static Engine │
                └──────┬────────┘
                       │
                 proof / counterexample
                       │
                       ▼
                 Review Agent
```

So:

**Static → LLM**

> “Here is a path from `recv_packet()` to `memcpy`, including alias information and constraints.”

**LLM → Static**

> “`decode_option()` appears to guarantee `len <= MAX_OPTION_LEN`; can you prove that this dominates the memcpy?”

**Static → LLM**

> “No. There exists the following feasible CFG path where that predicate is bypassed.”

**LLM**

> “Confirmed defect.”

This is far stronger than asking:

> “Do you think this code has a buffer overflow?”

---

# 3. This is exactly where current SOTA research is heading

There are roughly six highly interesting directions.

| Direction                                 | Static analysis contributes                    | LLM contributes                                          | Representative work     |
| ----------------------------------------- | ---------------------------------------------- | -------------------------------------------------------- | ----------------------- |
| **LLM-generated analysis specifications** | deterministic taint/dataflow engine            | identifies source/sink/sanitizer/API semantics           | IRIS, SemTaint          |
| **LLM-generated analysis rules/queries**  | executes scalable repository analysis          | generates/refines CodeQL-like queries                    | MoCQ, QLCoder           |
| **Static-analysis-guided LLM context**    | CPG/PDG/DFG/code slicing                       | reasons over the selected semantic slice                 | CPG+LM work             |
| **LLM false-positive triage**             | produces potential violations + paths          | judges application semantics/context                     | IRIS, Semgrep Assistant |
| **LLM-guided symbolic execution**         | mathematically explores feasible states        | creates harnesses/specs and chooses targets              | SAILOR                  |
| **LLM + formal verification**             | SMT/model checker gives proofs/counterexamples | builds formal model/specification and interprets results | CodeLogician            |

IRIS is particularly important conceptually. Instead of expecting an LLM to analyze an entire repository directly, it asks the LLM to infer project-specific taint specifications such as sources and sinks, lets CodeQL perform whole-repository dataflow analysis, then uses the LLM again to filter contextual false positives. On its CWE-Bench-Java evaluation, the authors report a substantial improvement over CodeQL alone, including large reductions in false-positive alerts under their context filtering configuration. ([arXiv][1])

**MoCQ takes the idea one step further:** the LLM extracts vulnerability patterns and translates them into analysis queries; the static-analysis side refines and executes those queries. The key insight is that the LLM becomes a **rule/specification synthesizer**, not the entity responsible for exhaustively interpreting program states. ([arXiv][2])

SemTaint is another particularly interesting 2026 example. It computes call-graph information using static analysis but delegates cases that are difficult to resolve statically, together with source/sink classification, to an LLM; those generated taint specifications are then passed back to CodeQL. ([arXiv][3])

---

# 4. I think “Program Analysis Guided RAG” will become fundamental

For large codebases, I would avoid conventional embedding RAG as the primary code retrieval mechanism.

Instead of:

```text
bug location
    ↓
embedding similarity
    ↓
20 semantically similar source files
```

use:

```text
bug location
    ↓
Program Dependence Graph
    ↓
backward slice
    +
forward slice
    +
call graph
    +
points-to graph
    +
change impact graph
    ↓
minimal causally relevant context
    ↓
LLM
```

This produces **causal context rather than textual context**.

A Code Property Graph can combine:

```text
AST
 +
CFG
 +
Call Graph
 +
DFG
 +
PDG
 +
Type graph
```

into the repository's structural knowledge base.

Recent work is explicitly exploring the bridge between CPG/PDG representations and language models, including using graph-derived data/control dependencies to constrain the amount of context exposed to the LLM. ([arXiv][4])

A particularly interesting recent result outside vulnerability detection is Athena, published in July 2026, which combines program-dependence information with learned conceptual representations for change-impact analysis. It is another signal that the promising direction is **graph semantics + neural semantics**, rather than source text alone. ([arXiv][5])

So for Cross-TU review I would make:

> **Program Graph = Agent's retrieval index**

rather than:

> Vector DB = Agent's retrieval index.

The vector DB can still exist, but mainly for documentation, requirements, design documents, historical reviews and similar semantic artifacts.

---

# 5. This also provides a much better solution to context-window problems

Suppose the suspicious operation is:

```cpp
memcpy(dst, packet->payload, packet->len);
```

There may be:

```text
20 MLOC repository
      ↓
87 callers
      ↓
1,900 possible call chains
      ↓
millions of source tokens
```

The LLM does not need those millions of tokens.

Static analysis may reduce this to:

```text
recv_packet()
   ↓
parse_frame()
   ↓
parse_extension()
   ↓
decode_ie()
   ↓
copy_ie()
       |
       +-- packet->len defined at A
       +-- validated at B
       +-- B does not dominate memcpy
       +-- dst capacity defined at C
```

And produce something like:

```json
{
  "sink": "memcpy",
  "source": "packet->len",
  "call_chain": [
    "recv_packet",
    "parse_frame",
    "parse_extension",
    "decode_ie",
    "copy_ie"
  ],
  "dataflow": "...",
  "path_constraints": [
    "type == EXTENSION",
    "version >= 2",
    "packet.len > dst.capacity"
  ],
  "dominators": [...],
  "relevant_lines": [...],
  "function_summaries": [...]
}
```

Now the Agent may need only **2–10 K meaningful tokens instead of hundreds of thousands of source tokens**.

This is one of the most important ways static analysis can make long-context code agents scalable.

---

# 6. LLMs should also help solve the “modeling problem” of static analysis

One of the deeper limitations of static analysis is not the analysis algorithm itself.

It is:

> **Who writes all the semantic models?**

For example:

```cpp
auto buf = framework.get_message();

validate(buf);

process(buf);
```

A generic analyzer may not know:

```text
framework.get_message()
       =
TAINT SOURCE

validate()
       =
SANITIZER
       only if
       validate() == SUCCESS
```

Large enterprise codebases have thousands of these internal APIs.

Today humans have to encode:

```text
source models
sink models
sanitizer models
ownership models
locking models
range contracts
error-code semantics
state-machine constraints
```

This is extremely expensive.

IRIS and SemTaint are good examples of a research direction in which LLMs infer such specifications and static analysis executes them deterministically. ([arXiv][1])

I think this will become one of the biggest practical wins:

```text
          Old world

Human
  ↓
write CodeQL/Semgrep rules
  ↓
Static Analyzer


          Future

Codebase + docs + API usage
          ↓
        Agent
          ↓
infer semantic model
          ↓
source / sink / sanitizer
range / lifetime / state contract
          ↓
generate static-analysis specification
          ↓
      Static Analyzer
```

The role of an expert therefore changes from:

> author every rule

to:

> review and govern automatically synthesized semantic models.

---

# 7. Conversely, static analysis should be a tool that the Agent calls

I would expose program-analysis operations as Agent tools:

```text
get_callers(function)

get_callees(function)

get_backward_slice(value)

get_forward_slice(value)

find_taint_paths(source, sink)

find_definition(value)

find_aliases(pointer)

prove_range(expr)

prove_dominates(A, B)

find_lockset(location)

get_function_summary(function)

is_path_feasible(path)

symbolically_execute(path)

find_state_transition(object)

query_codeql(query)
```

Then reasoning becomes:

```text
LLM:
"I suspect packet.len can exceed buf.capacity."

       ↓

prove_range(packet.len)

       ↓

Static engine:
0 <= packet.len <= 65535

       ↓

LLM:
"What constrains buf.capacity?"

       ↓

get_backward_slice(buf.capacity)

       ↓

Static engine:
buf.capacity = 4096

       ↓

LLM:
"Find whether len <= capacity dominates memcpy."

       ↓

prove_dominates(validation, memcpy)

       ↓

Static engine:
false
counterexample path = ...

       ↓

LLM:
Confirmed high-confidence overflow.
```

That is a much more powerful **Code Review Agent** than an LLM simply reading files.

---

# 8. Symbolic execution is another particularly promising integration point

Your path-explosion observation is especially relevant to path-sensitive symbolic analysis. One promising approach is to let program analysis identify likely targets and let the LLM synthesize the harness/specification required to explore them instead of blindly symbolically executing an entire system. The 2026 SAILOR work follows essentially this architecture: static analysis identifies candidate locations and generates vulnerability specifications; an LLM iteratively synthesizes drivers, stubs and assertions; symbolic execution then determines whether the candidate is realizable; finally, concrete replay validates the finding on the original program. ([arXiv][6])

The division of labor is elegant:

```text
LLM:
Where should we look?

Static Analysis:
What dependencies/path structure exists?

LLM:
How should we construct the analysis harness?

Symbolic Execution:
Is there actually a satisfying execution?

Concrete execution:
Can I reproduce it?
```

Instead of:

```text
LLM:
I'll guess whether the path is feasible.
```

---

# 9. Formal methods can become the Agent's “truth oracle”

Another promising 2026 direction is:

```text
Agent reasoning
      ↓
formal hypothesis
      ↓
SMT / Model Checker
      ↓
SAT / UNSAT / Counterexample
      ↓
Agent continues reasoning
```

CodeLogician, for example, combines an LLM agent with the Imandra automated reasoning system. The important architectural lesson is that formal reasoning is not merely a final validator; the LLM constructs explicit formal models and repeatedly calls the reasoning engine while answering questions about software behavior. The authors report large accuracy gains over LLM-only reasoning on their code-logic benchmark. ([arXiv][7])

I think that pattern is likely to become very important for safety-critical and telecom-grade software.

---

# 10. What industry products are doing now

GitHub's current Copilot Autofix architecture already demonstrates one side of this idea: CodeQL/code-scanning analysis identifies a specific issue and provides codebase/analysis context, while an LLM generates a targeted explanation and repair suggestion. ([GitHub Docs][8])

Semgrep is moving similarly. Its Assistant uses AI for alert triage/noise reduction, while its 2026 Autofix work combines static-analysis findings with frontier models to produce contextual remediation. Semgrep also describes multimodal detection combining static analysis with AI reasoning for issues such as business-logic flaws that are difficult to encode entirely in conventional rules. ([Semgrep][9])

These commercial systems are significant, but my assessment from the current product and research landscape is that we are still relatively early: mainstream products predominantly use the LLM for **triage/explanation/remediation**, while research prototypes increasingly investigate the deeper **LLM ⇄ program-analysis feedback loop**. ([GitHub Docs][8])

---

# 11. One rule I would enforce very strictly: LLMs must not silently destroy soundness

This matters particularly in large embedded/telecom software.

Suppose static analysis says:

```text
Potential UAF
confidence = medium
```

and the LLM says:

```text
This seems safe because foo() normally retains ownership.
```

Do **not** turn that into:

```text
FALSE POSITIVE
```

unless something deterministic verifies the assertion.

Use:

```text
Analyzer warning
       +
Agent semantic judgment
       ↓

 ┌─────────────────────────┐
 │ Verified Safe           │ <- proof available
 │ Verified Defect         │ <- counterexample available
 │ Likely Defect           │
 │ Likely False Positive   │
 │ Unknown                 │
 └─────────────────────────┘
```

Thus there are really two operating modes:

```text
ASSURANCE MODE

Static analyzer
     ↓
LLM may rank/explain
     ↓
only formal/static evidence
can suppress a finding
```

versus

```text
DISCOVERY MODE

LLM
 ↓
aggressively infer
 ↓
hypothesis / new rule
 ↓
static analysis validates
```

This separation is extremely important.

---

# 12. For your kind of large Cross-TU/WPA system, I would implement it this way

My recommended architecture is:

```text
                 Code Intelligence Platform
                           │
       ┌───────────────────┼───────────────────┐
       │                   │                   │
      AST                 CPG                 Build
      SSA           CFG/DFG/PDG/VFG       dependency graph
       │                   │                   │
       └───────────────────┼───────────────────┘
                           │
                  Function Summary DB
                           │
         ┌─────────────────┴─────────────────┐
         │                                   │
Incremental WPA                       Demand-driven WPA
         │                                   │
         └─────────────────┬─────────────────┘
                           │
                    Analysis Evidence IR
                           │
             ┌─────────────▼─────────────┐
             │      Review Agent         │
             │                           │
             │ Semantic reasoning        │
             │ Rule synthesis            │
             │ Contract inference        │
             │ FP triage                 │
             │ Impact reasoning          │
             │ Fix generation            │
             └─────────────┬─────────────┘
                           │
                    Proof obligations
                           │
         ┌─────────────────┼─────────────────┐
         │                 │                 │
      SAST/Query           SMT          Test/Fuzz/SE
         │                 │                 │
         └─────────────────┼─────────────────┘
                           ↓
                    Evidence Judge
                           │
                           ↓
                       PR Review
```

For scalability, **Function Summary DB** becomes critical.

Instead of repeatedly analyzing:

```text
A → B → C → D → E → ...
```

store summaries such as:

```text
foo():

READ:
    ctx->state

WRITE:
    ctx->counter

TAINT:
    arg0 → ret

RANGE:
    ret ∈ [0, 1023]

OWNERSHIP:
    arg1 consumed

LOCK:
    requires ctx->mutex

STATE:
    IDLE → ACTIVE

ERROR:
    -EINVAL if len > MAX_LEN
```

Cross-TU analysis then becomes much more manageable.

---

# 13. I would go even further for telecom/embedded software

For large RAN-like systems, traditional SAST only captures part of what you actually want to review.

I would extend AEIR beyond:

```text
CFG
DFG
VFG
alias
```

to include:

```text
Protocol State Graph
Task/Thread Graph
Lock Dependency Graph
Memory Ownership Graph
Message/Dataflow Graph
Interrupt Context Graph
Real-time Deadline Graph
Hardware Resource Graph
Feature Dependency Graph
```

Then a review Agent could reason about questions such as:

```text
Does this 6G change alter an existing 5G state transition?

Can this new call execute in interrupt context?

Does this new lock introduce inversion?

Can this path exceed the 1 ms scheduling budget?

Does changing this message affect another processing core?

Is an object freed by Core-0 while still referenced by Core-3?
```

Most of these cannot be solved well using either LLM-only review **or** conventional AST-rule SAST.

But:

> **domain semantic graph + deterministic program analysis + LLM reasoning**

has a realistic chance.

---

# 14. My strongest recommendation

If I were designing this system, I would not call the project simply:

**AI Code Review Agent**

I would position it as:

> ## Neuro-Symbolic Software Analysis Platform

with three distinct layers:

```text
L3   Semantic Reasoning
     ───────────────────────────
     LLM / Agent
     intent / architecture /
     protocol / domain reasoning

                 ▲ ▼

L2   Program Knowledge
     ───────────────────────────
     CPG / PDG / VFG
     Function Summary
     Analysis Evidence IR
     Semantic Models

                 ▲ ▼

L1   Deterministic Reasoning
     ───────────────────────────
     Rule Engine
     Dataflow / Taint
     Abstract Interpretation
     Symbolic Execution
     SMT / Model Checking
```

And the fundamental principle should be:

> **LLM decides what needs to be reasoned about.
> Static analysis determines what the program can do.
> Formal/dynamic verification determines whether the hypothesis is actually true.**

