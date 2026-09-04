> **Superseded design input.** This brainstorm predates and is superseded by the
> approved [M8R Soufflé WPA remediation design spec](../specs/milestones/m08r-souffle-wpa-remediation-design-spec.md),
> which is authoritative for current WPA ownership. The original analysis below
> is retained unchanged for reference.

Soufflé is explicitly designed for large-scale static analysis: its input relations form the EDB, its rules derive IDB relations to a fixed point, and it can compile a Datalog specification into native parallel C++. It also provides components, provenance, profiling/auto-scheduling, SQLite I/O, and a C++ embedding interface. ([Soufflé][1])

My main idea is:

> **Do not define the system as “LLVM IR → Soufflé facts”. Define a stable Relational Program IR between LLVM and all analyses.**

That intermediate relational schema will become the real long-term infrastructure.

---

# 1. Recommended overall architecture

I would build it as four logical layers:

```text
                        Build System
                clang / clang++ / rustc / ...
                           │
                           ▼
              ┌────────────────────────┐
              │ LLVM Capture Frontend  │
              │ .bc / modules / link   │
              │ debug info / DataLayout│
              └───────────┬────────────┘
                          │
                          ▼
        ┌─────────────────────────────────────┐
        │ LLVM → Relational Program IR        │
        │                                     │
        │ L0 Raw LLVM Facts                   │
        │ L1 Normalized Semantic Facts        │
        │ L1H LLVM Helper Analysis Facts      │
        └────────────────┬────────────────────┘
                         │
                         │ EDB
                         ▼
             ┌─────────────────────────┐
             │ Soufflé Analysis Kernel │
             │                         │
             │ Points-to + CallGraph   │
             │ Alias / Memory Model    │
             │ Constant/Range          │
             │ Reachability            │
             │ Taint / Typestate       │
             │ Security Rules          │
             └──────────┬──────────────┘
                        │ IDB
                        ▼
             ┌─────────────────────────┐
             │ Analysis Fact Bus       │
             │                         │
             │ PointsTo                │
             │ CallEdge                │
             │ Alias                   │
             │ ValueFlow               │
             │ TaintFlow               │
             │ Constant                │
             │ Finding / Evidence      │
             └──────────┬──────────────┘
                        │
        ┌───────────────┼────────────────┐
        ▼               ▼                ▼
  More Soufflé     C++ analysis      Agent/LLM/
    analyses          passes         review layer
```

The critical idea is:

```text
LLVM ≠ your analysis IR

LLVM
   ↓ lowering
Relational Program IR
   ↓ declarative analysis
Analysis Relations
   ↓
higher-order analysis
```

cclyzer++ is useful evidence that this general approach works: it uses an LLVM pass as a fact generator and Soufflé for a global pointer analysis; it supports field/array sensitivity, on-the-fly call-graph construction and configurable context sensitivity. Its own fact generator is intentionally close to a table-oriented view of the LLVM module. ([Galois Inc.][2])

I would go further than cclyzer++, however, and explicitly establish a normalized semantic layer.

---

# 2. The most important technology: the Relational Program IR

I would call this something like:

**RIR — Relational Intermediate Representation**

or more specifically:

**RPIR — Relational Program Intermediate Representation**

It should not be specific to pointer analysis.

Its job is to make these analyses possible from the same data:

```text
                RPIR
                  │
        ┌─────────┼────────────┐
        ▼         ▼            ▼
      PTA      Constant      Taint
        │      Propagation      │
        │         │             │
        └────┬────┴────┬────────┘
             ▼         ▼
        CallGraph    ValueFlow
             │         │
             └────┬────┘
                  ▼
             Security Rules
```

The mistake to avoid is designing EDB relations such as:

```text
AndersenAssign(...)
AndersenLoad(...)
AndersenStore(...)
```

Those already encode one particular analysis.

Instead use semantic relations:

```text
Copy
Load
Store
AddressOf
GEP
Call
ActualArg
FormalArg
Return
AllocObject
```

Then Andersen, Steensgaard, taint, escape analysis, value-flow and other analyses can all consume the same facts.

---

# 3. Use four levels of facts

## L0 — Raw LLVM facts

This layer should be almost lossless.

Examples:

```text
Module
Function
Global
Argument
BasicBlock
Instruction
Operand
Type
Constant
Metadata
Attribute
```

For example:

```souffle
.decl Instruction(
    inst: InstructionId,
    function: FunctionId,
    block: BlockId,
    opcode: OpcodeId,
    sequence: unsigned
)

.decl Operand(
    inst: InstructionId,
    index: unsigned,
    value: ValueId
)

.decl ValueType(
    value: ValueId,
    type: TypeId
)

.decl CFGEdge(
    src: BlockId,
    dst: BlockId,
    kind: EdgeKind
)
```

L0's purpose is:

> if an analysis needs some LLVM property later, you should not have to modify the LLVM extractor unless LLVM itself did not provide that property.

---

# 4. L1 — normalized semantic facts

This is where the architecture becomes much more powerful.

LLVM:

```llvm
%x = bitcast ptr %a to ptr
%y = getelementptr %struct.S, ptr %x, i64 0, i32 3
%z = load ptr, ptr %y
```

should have raw representation in L0, but also normalized relations such as:

```text
PointerCast(x, a)

Gep(
    result = y,
    base = x,
    fieldPath = S.field3
)

Load(
    instruction,
    result = z,
    address = y
)
```

Likewise:

```llvm
store ptr %v, ptr %p
```

becomes:

```text
Store(inst, address=p, value=v)
```

and:

```llvm
%p = alloca %struct.foo
```

becomes:

```text
StackAllocation(object, inst)
AddressOf(resultValue, object)
```

This gives you a simple analysis vocabulary:

```text
AddressOf(V,O)
Copy(D,S)
Load(D,P)
Store(P,S)
Gep(D,B,F)
Alloc(V,O)
```

That vocabulary becomes the "instruction set" of your static-analysis engine.

---

# 5. Why I would not make LLVM instructions themselves the primary abstraction

LLVM is SSA-based, which is highly useful for value-flow analysis, but memory remains a separate problem. LLVM explicitly treats globals as memory regions accessed through pointers, and LLVM's MemorySSA exists precisely to overlay SSA-like use/def structure on memory operations. ([LLVM][3])

For example:

```llvm
%p = alloca i32
store i32 10, ptr %p
%x = load i32, ptr %p
```

SSA gives you:

```text
10 ──store──> memory
memory ──load──> %x
```

but not a direct SSA edge:

```text
10 → %x
```

That edge depends upon:

* alias analysis,
* memory-object abstraction,
* memory locations,
* reaching stores,
* calls that may modify memory,
* control flow.

This is exactly where your Soufflé engine becomes valuable.

---

# 6. Export LLVM helper analyses too

There is an important architectural nuance here.

I would **not** force Soufflé to recompute everything that LLVM already computes very efficiently.

Export selected helper facts such as:

```text
Dominator
ImmediateDominator
PostDominator
LoopContains
LoopHeader
MemorySSADef
MemorySSAUse
MemorySSAPhi
MemorySSAClobber
```

LLVM MemorySSA already builds `MemoryDef`, `MemoryUse`, and `MemoryPhi` nodes over memory operations and exposes use-def/clobber relationships. It is intraprocedural and intentionally trades some precision for speed, so I would treat it as useful structural evidence rather than the definitive interprocedural memory model. ([LLVM][4])

This gives your representation:

```text
             LLVM
               │
     ┌─────────┴──────────┐
     ▼                    ▼
raw instructions       LLVM analyses
     │                    │
     └──────────┬─────────┘
                ▼
              RPIR
```

Good candidates to export from LLVM rather than recompute include:

* CFG;
* dominator/post-dominator trees;
* LoopInfo;
* DataLayout calculations;
* GEP constant byte offsets;
* source/debug mappings;
* MemorySSA;
* selected attributes/effects.

But **points-to, whole-program call graph and your central semantic memory abstraction should remain under your analysis architecture**, rather than simply trusting LLVM AA.

---

# 7. Whole-program representation: do not make one giant `llvm-link` module mandatory

This is another important design decision.

A prototype can absolutely do:

```text
TU1.bc
TU2.bc
TU3.bc
   │
llvm-link
   ▼
whole.bc
   │
FactGenerator
```

PhASAR, for example, documents WLLVM/GLLVM as a method for obtaining whole-program LLVM bitcode for complex projects. ([GitHub][5])

But I would not make this your long-term architecture.

Instead:

```text
TU-A.bc ──► facts A ┐
TU-B.bc ──► facts B ├──► Logical Whole Program
TU-C.bc ──► facts C ┘
                       +
                   Link facts
```

Use relations such as:

```text
Module(M)

ModuleDefines(M, F)
ModuleDeclares(M, F)

GlobalSymbol(F, Name)

Linkage(F, "external")
Visibility(F, "default")

ResolvedSymbol(Symbol, Definition)

ComdatMember(...)
WeakDefinition(...)
AliasDefinition(...)
```

This gives three major benefits.

### Incrementality

Changing one `.cpp` file only invalidates one module's facts.

### Better provenance

Every LLVM entity still belongs to its original compilation unit.

### Correct linker semantics

You can model:

```text
weak
linkonce_odr
available_externally
COMDAT
aliases
IFUNC
archives
dynamic libraries
```

instead of assuming concatenating IR is equivalent to the final program.

For an MVP, monolithic bitcode is easier.

For a production system, use a **module-set EDB + linker resolution layer**.

---

# 8. Stable IDs are extremely important

Do not use LLVM textual names as your primary IDs:

```text
"%27"
"%tmp"
"foo"
```

These are unstable and expensive join keys.

Use dense integer IDs internally:

```souffle
.type ValueId       <: unsigned
.type InstId        <: unsigned
.type FunctionId    <: unsigned
.type ObjectId      <: unsigned
.type FieldId       <: unsigned
.type CallSiteId    <: unsigned
```

Keep human-readable information separately:

```text
FunctionName(FunctionId, symbol)
SourceLocation(InstId, fileId, line, column)
PrettyLLVM(ValueId, text)
```

So the hot relation is:

```text
VarPointsTo(17791, 8102)
```

rather than:

```text
VarPointsTo(
  "foo::bar()/%tmp.19",
  "*heap_alloc@foo::bar():91"
)
```

Soufflé itself interns symbols internally, but numerical domains still simplify large graph-style schemas and make cross-run storage much cleaner. Soufflé supports typed subtypes, which is useful for preventing accidental joins between e.g. a `FunctionId` and `ObjectId`. ([souffle-lang.com][6])

---

# 9. Use two kinds of identity

For incremental analysis I would distinguish:

```text
NodeId
StableId
```

### NodeId

Dense identifier for a specific program snapshot.

Excellent for analysis:

```text
10001
10002
10003
```

### StableId

Best-effort identity across revisions.

For example:

```text
FunctionStableId =
    hash(module + mangled-name + signature)

InstructionStableId =
    hash(
       FunctionStableId,
       source-anchor,
       semantic-fingerprint,
       local-occurrence
    )
```

Then:

```text
StableMapping(
    Revision,
    StableId,
    NodeId
)
```

The analysis only uses `NodeId`.

Your incremental infrastructure uses `StableId`.

Do not try to make one identifier perform both jobs.

---

# 10. The memory-object model is probably your hardest semantic component

For sophisticated LLVM analysis I would define:

```text
AbstractObject
    =
    GlobalObject
  | StackObject
  | HeapObject
  | FunctionObject
  | ExternalObject
  | UnknownObject
```

and eventually:

```text
AbstractLocation =
    AbstractObject × AccessPath
```

Example:

```c
struct A {
    int x;
    struct B {
        int y;
        char *p;
    } b;
};
```

represent:

```text
objA
objA.x
objA.b
objA.b.y
objA.b.p
```

not only:

```text
objA
```

So:

```text
SubObject(objA, fld_x,       objA.x)
SubObject(objA, fld_b,       objA.b)
SubObject(objA.b, fld_y,     objA.b.y)
SubObject(objA.b, fld_p,     objA.b.p)
```

This is essential for useful taint analysis.

Otherwise:

```c
p->trusted
p->untrusted
```

collapse into the same memory location.

---

# 11. Do not rely entirely on LLVM type information

This is especially important with modern LLVM opaque pointers.

Old LLVM:

```llvm
i32*
struct.foo*
```

Modern LLVM:

```llvm
ptr
```

Therefore your memory representation needs information from:

```text
GEP source type
load/store operand type
DataLayout
debug types
TBAA metadata
allocation size
source-level debug information
```

PhASAR explicitly notes that after moving to opaque pointers, debug information becomes important for precise/scalable analysis. Meanwhile older cclyzer++ releases were built around older LLVM versions and did not fully support opaque pointers, so its schema should be treated as inspiration rather than copied verbatim. ([GitHub][7])

I would represent location identity primarily in terms of:

```text
Object
+
byte range
+
semantic field path when known
```

rather than:

```text
Object + LLVM pointee type
```

For example:

```text
MemoryLocation(
    object,
    offsetLow,
    offsetHigh,
    fieldPath
)
```

This also handles:

```c
union
char*
memcpy
reinterpret_cast
type punning
```

much better.

---

# 12. Core points-to EDB

The normalized input to points-to analysis could be astonishingly small:

```souffle
.decl AddressOf(v:ValueId, obj:ObjectId)
.decl Copy(dst:ValueId, src:ValueId)

.decl Load(
    dst:ValueId,
    ptr:ValueId
)

.decl Store(
    ptr:ValueId,
    src:ValueId
)

.decl Gep(
    dst:ValueId,
    base:ValueId,
    field:FieldId
)
```

Then derive:

```souffle
.decl VarPointsTo(v:ValueId, obj:ObjectId)
.decl MemPointsTo(base:ObjectId, field:FieldId, obj:ObjectId)
```

Conceptually:

```souffle
VarPointsTo(V,O) :-
    AddressOf(V,O).

VarPointsTo(D,O) :-
    Copy(D,S),
    VarPointsTo(S,O).
```

For stores:

```text
*p = q
```

derive:

```text
VarPointsTo(p,o1)
VarPointsTo(q,o2)

=>

MemPointsTo(o1,*,o2)
```

For loads:

```text
q = *p
```

derive:

```text
VarPointsTo(p,o1)
MemPointsTo(o1,*,o2)

=>

VarPointsTo(q,o2)
```

The official Soufflé documentation includes the same general typed-relation pattern for simple points-to analysis. ([souffle-lang.com][8])

---

# 13. Points-to and call graph must be solved together

Suppose:

```c
fp(x);
```

The callee depends on:

```text
PointsTo(fp)
```

But points-to propagation across the call depends on knowing the callee.

So:

```text
        PointsTo
           │
           ▼
       CallGraph
           │
           ▼
 parameter/return flow
           │
           └────────► PointsTo
```

This is a genuine mutual fixed point.

Model:

```text
DirectCall(Callsite, F)

IndirectCalledValue(Callsite, V)

FunctionObject(Object, F)
```

then:

```souffle
CallEdge(C,F) :-
    DirectCall(C,F).

CallEdge(C,F) :-
    IndirectCalledValue(C,V),
    VarPointsTo(V,O),
    FunctionObject(O,F).
```

and:

```text
ActualParameter(C,i,A)
FormalParameter(F,i,P)
```

allows:

```text
A → P
```

once:

```text
CallEdge(C,F)
```

exists.

This should live in **one foundational Soufflé fixed point**.

---

# 14. Context sensitivity should be a parameter, not another implementation

Conceptually:

```text
VarPointsTo(Context, Value, HeapContext, Object)
```

instead of:

```text
VarPointsTo(Value,Object)
```

The context abstraction can be:

```text
CI
1-call-site
2-call-site
object-sensitive
type-sensitive
hybrid
```

cclyzer++ takes this approach and represents contexts with Soufflé records, with configurable k-callsite sensitivity and heap cloning. ([Galois Inc.][9])

I would define:

```text
Context =
    CallString
```

with:

```text
PushContext(CurrentCtx, CallSite, NewCtx)
```

as a reusable analysis component.

Then points-to rules don't need to know the exact context policy.

---

# 15. Heap context matters separately

For:

```c
T *factory() {
    return malloc(sizeof(T));
}

a = factory();
b = factory();
```

allocation-site analysis yields:

```text
a → malloc@factory
b → malloc@factory
```

whereas heap cloning can yield:

```text
a → malloc@factory/context1
b → malloc@factory/context2
```

Therefore define:

```text
HeapObject =
    AllocationSite × AllocationContext
```

separately from:

```text
ExecutionContext
```

This distinction becomes important for high-precision taint analysis.

---

# 16. External function models are part of the semantic foundation

You cannot achieve useful whole-program analysis simply by analysing project IR.

Consider:

```c
memcpy(dst, src, size);
strcpy(dst, src);
read(fd, buf, len);
recv(fd, buf, len);
malloc(n);
pthread_create(...);
qsort(... callback ...);
```

The definitions may not exist in your LLVM IR.

Create a **Function Summary Model DB**:

```text
FunctionModel
```

with effects such as:

```text
Allocates(return)
Copies(arg1 → memory(arg0))
Reads(memory(arg1))
Writes(memory(arg0))
Taints(memory(arg1))
ReturnsAlias(arg0)
Captures(arg2)
InvokesCallback(arg3)
```

Example:

```text
ModelMemcpy:
    read memory(arg1)
    write memory(arg0)
    value-flow memory(arg1) → memory(arg0)

ModelMalloc:
    allocate object
    return → object

ModelRead:
    source(external-input)
    memory(arg1) ← source
```

cclyzer++ also treats external-function signatures as crucial to soundness. ([Galois Inc.][10])

This model DB becomes as important as the Datalog rules themselves.

---

# 17. Build a canonical Value-Flow Graph after points-to

Once points-to stabilizes, generate a generic:

```text
ValueFlowEdge(
    From,
    To,
    EdgeKind
)
```

where `EdgeKind` could be:

```text
SSA
Load
Store
Parameter
Return
Phi
Select
GEP
Memcpy
GlobalInitializer
```

Conceptually:

```text
           ┌──── SSA ───────┐
           │                │
           ▼                ▼
        Value ────────► Value
           │
          store
           ▼
        Memory
           │
          load
           ▼
        Value
           │
        argument
           ▼
      FormalParameter
           │
           ▼
         Callee
           │
         return
           ▼
         Caller
```

That VFG then becomes the substrate for:

```text
taint
slicing
dependency analysis
security rules
impact analysis
data lineage
```

This is a major reuse point.

---

# 18. Taint becomes very simple once VFG exists

EDB:

```text
TaintSource(Node, TaintKind)
TaintSink(Node, TaintKind)
Sanitizer(Node, TaintKind)

ValueFlowEdge(From, To, Kind)
```

IDB:

```souffle
Tainted(V,K) :-
    TaintSource(V,K).

Tainted(B,K) :-
    Tainted(A,K),
    ValueFlowEdge(A,B,_),
    !BlockedFlow(A,B,K).
```

Then:

```text
Finding(Source, Sink, Kind)
```

when a tainted value reaches an incompatible sink.

The difficult part of taint analysis is therefore usually not:

```text
reachability
```

but:

```text
precise ValueFlowEdge construction
```

which in turn depends heavily on:

```text
points-to + memory modeling + call graph
```

So architecturally:

```text
PTA
 ↓
Memory dependence
 ↓
VFG
 ↓
Taint
```

is preferable to each taint rule independently interpreting LLVM loads and stores.

---

# 19. Constant propagation needs a lattice

This analysis is structurally different.

Use a lattice:

```text
        TOP / Overdefined
          /        \
       C1            C2
          \        /
           UNDEF
```

Conceptually:

```text
UNDEF
CONST(10)
CONST(20)
OVERDEFINED
```

and:

```text
merge(CONST(10),CONST(10))
    = CONST(10)

merge(CONST(10),CONST(20))
    = OVERDEFINED
```

Soufflé's subsumption feature is useful here because it allows a more general tuple to dominate/delete a more specific tuple according to a defined partial order. ([Soufflé][11])

I would represent:

```text
ConstState(Value, Kind, Constant)
```

where:

```text
Kind =
    UNDEF
    CONST
    OVERDEFINED
```

and use subsumption to retain the dominating lattice state.

This pattern can later support:

```text
range propagation
nullness
sign analysis
integer domain analysis
string abstractions
```

---

# 20. Add executable-edge propagation

For useful constant propagation you also need branch feasibility.

Example:

```c
if (x == 10)
    y = 3;
else
    y = 4;
```

If:

```text
x = 10
```

you don't want both:

```text
y=3
y=4
```

So add:

```text
ExecutableBlock
ExecutableEdge
```

similar conceptually to sparse conditional constant propagation:

```text
Constant lattice
       +
CFG executability
```

Then:

```text
Constant(Value)
```

and:

```text
ExecutableEdge(B1,B2)
```

evolve together.

This gives you a second reusable foundation:

```text
FeasibleCFG
```

which can improve security analysis dramatically.

---

# 21. Be careful coupling constant propagation back into points-to

There is an architectural trap here.

Initially:

```text
PTA → Const → Taint
```

looks sufficient.

But constants can eliminate branches:

```text
Const
 ↓
Feasible CFG
 ↓
more precise PointsTo
```

which creates:

```text
PointsTo
   ↓
Constant
   ↓
CFG pruning
   ↓
PointsTo
```

The problem is that ordinary Datalog works particularly naturally for **monotonically accumulating may-information**. Removing a previously feasible edge is a retraction.

Therefore I would start with stratified analysis:

```text
Stage A:
conservative PointsTo + CallGraph

Stage B:
Constant / Range + Feasibility

Stage C:
Refined ValueFlow

Stage D:
Taint / security
```

Do not immediately create a giant cyclic meta-analysis.

You can introduce higher-order iterative refinement later.

---

# 22. Analysis results should have a first-class schema

Do not treat `.csv` output as an incidental file.

Define an **Analysis Fact Bus**.

For example:

```text
AnalysisRun(
    RunId,
    RevisionId,
    AnalysisName,
    RuleVersion,
    ConfigurationHash
)
```

Then:

```text
VarPointsTo(
    RunId,
    Context,
    Value,
    HeapContext,
    Object
)

CallEdge(
    RunId,
    Context,
    CallSite,
    Callee
)

ConstantValue(
    RunId,
    Context,
    Value,
    Constant
)

TaintFact(
    RunId,
    Context,
    Node,
    Label
)
```

This means every result is reproducible.

You can answer:

```text
Which rule set?
Which context sensitivity?
Which library model version?
Which code revision?
Which LLVM version?
```

produced this result.

That is extremely important for production static analysis.

---

# 23. IDB can become EDB

This directly addresses your requirement:

> Soufflé analysis results as input for further analysis.

A clean model is:

```text
Analysis A

EDB-A
  +
Rules-A
  ↓
IDB-A
```

then materialize selected relations:

```text
IDB-A
  ↓
Analysis Fact Bus
  ↓
EDB-B
```

For example:

```text
LLVM EDB
   │
   ▼
PointsTo.dl
   │
   ├── VarPointsTo
   ├── MemPointsTo
   └── CallEdge
           │
           ▼
        VFG.dl
           │
           └── ValueFlowEdge
                    │
                    ▼
                 Taint.dl
                    │
                    ▼
                 Finding
```

But there is a useful optimization:

If stages belong to the same fixed point, don't serialize them.

Use:

```text
points_to.dl
callgraph.dl
memory.dl
```

as components in the same Soufflé executable.

Soufflé's component model is intended for structuring larger logic programs, and cclyzer++ uses components to share implementations between pointer-analysis variants. ([Soufflé][12])

---

# 24. Proposed Soufflé source organization

I would use:

```text
logic/
│
├── schema/
│   ├── types.dl
│   ├── llvm_raw.dl
│   ├── semantic_ir.dl
│   ├── memory.dl
│   └── models.dl
│
├── foundation/
│   ├── cfg.dl
│   ├── callgraph.dl
│   ├── points_to.dl
│   ├── contexts.dl
│   ├── memory_objects.dl
│   └── value_flow.dl
│
├── domains/
│   ├── constant.dl
│   ├── range.dl
│   ├── nullness.dl
│   └── taint_domain.dl
│
├── security/
│   ├── taint.dl
│   ├── use_after_free.dl
│   ├── double_free.dl
│   ├── command_injection.dl
│   └── ...
│
├── models/
│   ├── libc.dl
│   ├── libstdcpp.dl
│   ├── posix.dl
│   └── project.dl
│
└── projects/
    ├── foundation.project
    ├── security.project
    └── debug.project
```

Do not build one giant `analysis.dl`.

---

# 25. Design the system around provenance from day one

A static analyzer that says:

```text
WARNING:
user-controlled input reaches memcpy
```

is not enough.

You need:

```text
Source:
  read(fd, buf)

↓ call argument

parse_packet(buf)

↓ store

packet->payload

↓ load

copy_payload()

↓ argument

memcpy(dst, packet->payload, len)
```

Soufflé has built-in provenance support and can produce proof trees for derived tuples. ([Soufflé][13])

But I would not expose raw Datalog proof trees directly as the user-facing explanation.

Instead produce an explicit:

```text
EvidenceEdge(
    Result,
    Cause,
    Rule,
    ProgramNode
)
```

layer.

For example:

```text
Finding #9182
 │
 ├── SinkCall memcpy@foo.cpp:283
 │
 ├── Taint #717
 │    ├── VFG edge
 │    ├── VFG edge
 │    └── Source read@network.cpp:91
 │
 └── CallEdge
      └── PointsTo
```

Now results can be consumed by:

* IDE;
* code-review UI;
* security dashboard;
* another static analyzer;
* an LLM/agent.

---

# 26. EDB schema I would initially implement

A practical first version would contain roughly these groups:

| Domain      | Key relations                              |
| ----------- | ------------------------------------------ |
| Project     | Module, CompilationUnit, Revision          |
| Symbols     | Function, Global, Alias, IFunc             |
| CFG         | BasicBlock, CFGEdge, Entry, Exit           |
| SSA         | Instruction, Defines, Operand, PhiIncoming |
| Types       | Type, StructField, TypeSize, TypeAlign     |
| Memory      | Load, Store, Alloca, Atomic, MemIntrinsic  |
| Pointer     | GEP, Cast, PtrToInt, IntToPtr              |
| Calls       | CallSite, CalledValue, ActualArg, Return   |
| Globals     | Initializer, GlobalField                   |
| Linkage     | Linkage, Visibility, COMDAT, Resolution    |
| Attributes  | FunctionAttr, ArgAttr, MemoryEffects       |
| Debug       | SourceLocation, Variable, LexicalScope     |
| LLVM helper | Dominator, Loop, MemorySSA                 |
| Models      | Allocator, Copier, Source, Sink, Sanitizer |

This is probably on the order of **50–100 relations**, not thousands.

That's a reasonable foundation.

---

# 27. Special LLVM cases you should explicitly design for

These will otherwise become repeated precision bugs:

```text
phi
select
GEP
bitcast
addrspacecast
ptrtoint/inttoptr

memcpy
memmove
memset

atomicrmw
cmpxchg
volatile
fence

invoke
landingpad
catchswitch
resume

varargs

global initializers

function aliases
IFUNC

function pointers

C++ vtables
RTTI
virtual dispatch

callbacks

TLS

inline asm

dlopen/dlsym

coroutines

custom allocators
placement new
```

Have a relation:

```text
UnsupportedSemanticFeature(...)
```

rather than silently ignoring them.

That one relation will save a lot of debugging.

---

# 28. Define explicit soundness policies

For every unsupported construct, decide among:

```text
SoundConservative
BestEffort
UnsoundIgnore
AnalysisAbort
```

For example:

```text
inline asm:
    default = conservative memory clobber

unknown external function:
    may read reachable memory
    may write pointer arguments
    may capture pointers
```

A production static analyzer should be able to report:

```text
SoundnessCoverage:
  97.4% functions fully modeled
  2.1% conservative fallback
  0.5% unsupported
```

This is far more useful than claiming "whole-program analysis" without qualification.

---

# 29. Incremental analysis is the major architectural issue after correctness

Standard Soufflé is very good at compiling a fixed logic program into an efficient analyzer, but its normal execution model is still essentially:

```text
load EDB
compute fixed point
emit results
```

and its synthesis documentation explicitly notes that it does not provide persistent storage internally. ([Soufflé][14])

For a million-line project:

```text
1 file changes
     ↓
re-extract entire project
     ↓
re-run entire WPA
```

will eventually be unacceptable.

Therefore design your EDB now as:

```text
EDB(Module, Revision)
```

even if version 1 simply recomputes everything.

---

# 30. Incremental architecture

Use:

```text
Source revision
      │
      ▼
Changed TUs
      │
      ▼
LLVM fact delta
   + facts
   - facts
      │
      ▼
Affected function/module graph
      │
      ▼
Re-analysis
```

Persistent storage might be:

```text
RocksDB
SQLite
DuckDB/Parquet
custom columnar store
```

while Soufflé receives a materialized working set.

Soufflé itself supports flat-file and SQLite I/O. ([souffle-lang.com][15])

I would therefore separate:

```text
Persistent Fact Store

        ≠

Soufflé working relations
```

This leaves room to evolve.

---

# 31. Interesting 2026 direction: Differential Datalog execution

There is also a very relevant recent research direction. The July 2026 FlowLog work compiles Soufflé-style Datalog to Differential Dataflow, targeting incremental static analysis; its demonstration includes retracting facts and updating analysis results incrementally. ([arXiv][16])

I would not immediately replace Soufflé with FlowLog—it's very recent research—but I would deliberately make the architecture:

```text
             Analysis specification
                     │
              Soufflé-style Datalog
                     │
       ┌─────────────┴──────────────┐
       ▼                            ▼
   Soufflé                     Incremental
 one-shot engine                engine later
```

possible.

This is another reason why the **EDB schema must be engine-independent**.

---

# 32. Performance rules for large whole-program analysis

A few design principles will matter enormously.

### Prefer IDs to strings in hot relations

Good:

```text
PointsTo(12381, 91822)
```

Avoid:

```text
PointsTo(
  "namespace::Class::foo/%129",
  "heap@..."
)
```

### Normalize high-cardinality relations

Avoid enormous relations containing:

```text
Instruction × Function × BB × opcode × type × location × ...
```

Split them.

Soufflé's own profiler documentation points out that poor data models and bad join schedules are common sources of bottlenecks. ([Soufflé][17])

### Avoid materializing every transitive relation

For example:

```text
Reachable(A,B)
```

over the entire VFG can explode quadratically.

Compute task-specific reachability instead.

Soufflé's magic-set transformation can specialize queries and reduce irrelevant intermediate tuples in suitable positive-Datalog cases. ([Soufflé][18])

### Compile production analyses

Development:

```text
souffle analysis.dl
```

Production:

```text
souffle -c ...
```

or pre-generate a native analyzer.

Soufflé can synthesize a Datalog specification into a specialized parallel C++ executable. ([Soufflé][14])

---

# 33. Integrate Soufflé into the C++ process eventually

During initial development:

```text
LLVM FactGenerator
     ↓
*.facts
     ↓
souffle
     ↓
*.csv
```

is perfect.

For the production architecture I would add:

```text
LLVM Analysis Runtime
        │
        ├── RPIR extractor
        │
        ├── Soufflé generated C++
        │
        └── Result API
```

Soufflé's C++ interface allows the host program to populate input relations programmatically, execute the generated analyzer and inspect output relations. ([Soufflé][19])

That gives:

```cpp
auto *pta = ProgramFactory::newInstance("PointsTo");

loadRPIR(pta, moduleFacts);

pta->run();

auto *pointsTo = pta->getRelation("VarPointsTo");
```

Eventually you can expose:

```cpp
PointsToResult queryPointsTo(const llvm::Value *);
CallGraphResult queryCallees(const llvm::CallBase *);
TaintResult queryTaint(const llvm::Value *);
```

rather than CSV.

---

# 34. My recommended analysis dependency graph

I would explicitly encode this DAG:

```text
                     LLVM RPIR
                        │
        ┌───────────────┼────────────────┐
        │               │                │
        ▼               ▼                ▼
       CFG           Type/Layout      Models
        │               │                │
        └───────┬───────┴───────┬────────┘
                ▼               ▼
          Object Model     Direct Calls
                │               │
                └──────┬────────┘
                       ▼
              ┌─────────────────┐
              │ Points-to + CG  │
              │ joint fixpoint  │
              └────────┬────────┘
                       │
             ┌─────────┴─────────┐
             ▼                   ▼
          Alias               Memory
                             Dependence
                                │
                                ▼
                            Value Flow
                                │
             ┌──────────────────┼──────────────────┐
             ▼                  ▼                  ▼
          Taint            Slicing            Typestate
                                                   
                 Constant / Range
                        │
                        ▼
                  Feasible CFG
                        │
                        ▼
              precision refinement
```

That is the architecture I would use as the basis for the entire project.

---

# 35. Most important components, in priority order

If I were starting implementation now, I would prioritize these ten technology blocks.

### P0 — RPIR specification

Define:

```text
entities
relations
types
semantics
IDs
versioning
```

before implementing sophisticated analyses.

### P0 — LLVM fact generator

Implement as a modern LLVM pass/plugin supporting opaque pointers and modern LLVM.

### P0 — memory/object abstraction

Define:

```text
allocation
subobject
field path
offset
overlap
unknown memory
```

This determines the ceiling of your later precision.

### P0 — function model infrastructure

Handle libc, libc++, system APIs and project-specific APIs.

### P1 — points-to + call graph

Start with context-insensitive Andersen.

Then:

```text
field sensitivity
heap abstraction
context sensitivity
```

incrementally.

### P1 — generic VFG

This should become the reusable backbone for many later analyses.

### P1 — provenance/evidence

Every finding should have a derivation path.

### P2 — taint framework

Make sources/sinks/sanitizers external policy data.

### P2 — constant/range lattice framework

Design it as a reusable abstract-domain mechanism.

### P2 — incremental fact infrastructure

Stable IDs, fact hashes, revisions and invalidation.

---

# 36. Development roadmap I would actually use

## Phase 1 — infrastructure

Support:

```text
C/C++
LLVM bitcode
Function/BB/instruction EDB
CFG
SSA def-use
Load/store/GEP
Call/return
Globals
Debug mapping
```

Validation:

```text
LLVM → EDB → reconstruct enough IR structure
```

Goal:

> prove that the RPIR is semantically complete.

---

## Phase 2 — context-insensitive pointer analysis

Implement:

```text
alloc
address-of
copy
load
store
GEP
direct call
indirect call
arguments
return
```

Outputs:

```text
VarPointsTo
MemPointsTo
CallEdge
Alias
```

Cross-validate against:

```text
cclyzer++
SVF
LLVM AA
```

where applicable.

---

## Phase 3 — memory precision

Add:

```text
field sensitivity
array sensitivity
byte-offset fallback
heap objects
custom allocators
memcpy/memmove
```

This will likely yield a larger real-world precision improvement than jumping immediately to deep context sensitivity.

---

## Phase 4 — VFG

Generate:

```text
ValueFlowEdge
MemoryFlowEdge
CallFlowEdge
ReturnFlowEdge
```

Now you have the reusable program-analysis graph.

---

## Phase 5 — taint

External configuration:

```text
source
sink
sanitizer
propagator
```

Produce:

```text
Taint
TaintPath
Finding
```

with source locations and evidence.

---

## Phase 6 — constant/range

Implement a reusable abstract-value framework:

```text
Constant
Range
Nullness
Sign
```

and executable-edge reasoning.

---

## Phase 7 — scalable context sensitivity

Introduce:

```text
1-callsite
2-callsite
heap cloning
selective contexts
```

Do **not** make `k=2 everywhere` the default.

Context explosion is often worse than the gain in precision. cclyzer++ likewise k-bounds contexts for practical termination/performance. ([Galois Inc.][20])

---

## Phase 8 — incrementality

Move from:

```text
ProgramRevision → complete recomputation
```

toward:

```text
Delta EDB
    ↓
affected region
    ↓
incremental/restricted analysis
```

This is where the architecture starts to become suitable for continuous code review rather than offline scanning.

---

# 37. One further architectural refinement I strongly recommend

Ultimately I would split the system into:

```text
             Program Knowledge Plane
                      │
                      │
    ┌─────────────────┼──────────────────┐
    │                 │                  │
 Static facts     Semantic facts    Analysis facts
    │                 │                  │
    ▼                 ▼                  ▼
LLVM structure     Object/VFG        PTA/Taint/etc.
```

In other words, Soufflé should not just be "an analyzer."

It becomes a **compiler for program knowledge**.

The flow is:

```text
program
   ↓
facts
   ↓
semantic relations
   ↓
analysis relations
   ↓
evidence
   ↓
higher reasoning
```

That is considerably more valuable than building three independent Datalog analyses.

---

# 38. The architecture I would target

Putting all of the pieces together:

```text
┌─────────────────────────────────────────────────────────────┐
│                         BUILD                               │
│ clang / rustc / linker / compile_commands                  │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                  LLVM CAPTURE LAYER                         │
│                                                             │
│ Module bitcode      Debug info        Link resolution       │
│ DataLayout          Attributes        External libraries    │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│             RELATIONAL PROGRAM IR — EDB                     │
│                                                             │
│ L0 LLVM                                                     │
│   Function / BB / Inst / Operand / Type / Metadata          │
│                                                             │
│ L1 Semantic                                                 │
│   Copy / Load / Store / GEP / Alloc / Call / Return         │
│                                                             │
│ LLVM Helpers                                                │
│   Dom / Loop / MemorySSA / Layout                           │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│              FOUNDATION ANALYSIS — SOUFFLÉ                  │
│                                                             │
│ Abstract Object Model                                       │
│          ↓                                                  │
│ Points-to ⇄ Call Graph                                      │
│          ↓                                                  │
│ Alias + Memory Dependence                                   │
│          ↓                                                  │
│ Value Flow Graph                                            │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                    ANALYSIS FACT BUS                        │
│                                                             │
│ PointsTo     Alias       CallEdge                           │
│ Constant     Range       ValueFlow                          │
│ Taint        Typestate   Reachability                       │
│ Finding      Evidence                                      │
└──────────────────────────┬──────────────────────────────────┘
                           │
          ┌────────────────┼─────────────────────┐
          ▼                ▼                     ▼
┌────────────────┐ ┌────────────────┐  ┌────────────────────┐
│ More Soufflé   │ │ C++ / IDE      │  │ Agent / semantic   │
│ analyses       │ │ queries        │  │ reasoning          │
└────────────────┘ └────────────────┘  └────────────────────┘
```

## The three things I would treat as the project's technical crown jewels

**1. The RPIR schema.** It determines whether analyses compose or become isolated implementations.

**2. The abstract memory/object model.** It determines your attainable precision for C/C++.

**3. The Analysis Fact Bus + evidence model.** It turns Soufflé output from disposable query results into reusable program knowledge.

The Datalog implementation itself is actually the easier part once these three are right.

A useful way to frame the entire project is therefore:

> **LLVM provides syntax and machine-level semantics; RPIR provides canonical program knowledge; Soufflé computes logical closure over that knowledge; the Analysis Fact Bus exposes the closure as reusable evidence for subsequent static and semantic reasoning.**

That architecture gives you a much stronger foundation than a conventional `LLVM → points-to.dl → CSV` pipeline, and it naturally supports the progression from deterministic static analysis toward higher-level semantic/security reasoning. ([Soufflé][21])

[1]: https://souffle-lang.github.io/docs.html?utm_source=chatgpt.com "Welcome | Soufflé • A Datalog Synthesis Tool for Static Analysis"
[2]: https://galoisinc.github.io/cclyzerpp/architecture.html "Architecture — cclyzer++ 0.1.0 documentation"
[3]: https://llvm.org/docs/LangRef.html?utm_source=chatgpt.com "LLVM Language Reference Manual - LLVM"
[4]: https://www.llvm.org/docs/MemorySSA.html?utm_source=chatgpt.com "MemorySSA - LLVM"
[5]: https://github.com/secure-software-engineering/phasar/wiki/Whole-Program-Analysis-%28using-WLLVM%29?utm_source=chatgpt.com "Whole Program Analysis (using WLLVM) · secure-software-engineering/phasar Wiki · GitHub"
[6]: https://www.souffle-lang.com/types?utm_source=chatgpt.com "Types | Soufflé • A Datalog Synthesis Tool for Static Analysis"
[7]: https://github.com/secure-software-engineering/phasar/wiki/Getting-Started%3A-Running-an-Analysis?utm_source=chatgpt.com "Getting Started: Running an Analysis · secure-software-engineering/phasar Wiki · GitHub"
[8]: https://www.souffle-lang.com/examples?utm_source=chatgpt.com "Examples | Soufflé • A Datalog Synthesis Tool for Static Analysis"
[9]: https://galoisinc.github.io/cclyzerpp/design.html?utm_source=chatgpt.com "Analysis Design — cclyzer++ 0.1.0 documentation"
[10]: https://galoisinc.github.io/cclyzerpp/overview.html?utm_source=chatgpt.com "Overview — cclyzer++ 0.1.0 documentation"
[11]: https://souffle-lang.github.io/subsumption?utm_source=chatgpt.com "Subsumption | Soufflé • A Datalog Synthesis Tool for Static Analysis"
[12]: https://souffle-lang.github.io/components?utm_source=chatgpt.com "Components | Soufflé • A Datalog Synthesis Tool for Static Analysis"
[13]: https://souffle-lang.github.io/provenance?utm_source=chatgpt.com "Provenance | Soufflé • A Datalog Synthesis Tool for Static Analysis"
[14]: https://souffle-lang.github.io/translate?utm_source=chatgpt.com "Synthesis | Soufflé • A Datalog Synthesis Tool for Static Analysis"
[15]: https://www.souffle-lang.com/directives?utm_source=chatgpt.com "Directives | Soufflé • A Datalog Synthesis Tool for Static Analysis"
[16]: https://arxiv.org/abs/2607.23971?utm_source=chatgpt.com "FlowLog: Re-thinking Datalog for Fast and Extensible Static Analysis"
[17]: https://souffle-lang.github.io/profiler?utm_source=chatgpt.com "Profiler | Soufflé • A Datalog Synthesis Tool for Static Analysis"
[18]: https://souffle-lang.github.io/magicset?utm_source=chatgpt.com "Magic-Set | Soufflé • A Datalog Synthesis Tool for Static Analysis"
[19]: https://souffle-lang.github.io/interface?utm_source=chatgpt.com "C++ Interface | Soufflé • A Datalog Synthesis Tool for Static Analysis"
[20]: https://galoisinc.github.io/cclyzerpp/implementation.html?utm_source=chatgpt.com "Analysis Implementation — cclyzer++ 0.1.0 documentation"
[21]: https://souffle-lang.github.io/?utm_source=chatgpt.com "Soufflé | Soufflé • A Datalog Synthesis Tool for Static Analysis"

