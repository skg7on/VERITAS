# M2 Identity, Canonical Hashing, and Metadata Store Design Spec

**Status:** Draft
**Milestone:** M2
**Depends on:** M1 build manifest
**Feeds:** M3 SummaryDB, M4 function extraction, all provenance and Evidence references

---

# 1. Purpose

M2 establishes VERITAS's stable identity system. It turns M1's manifest into persistent repository, revision, build variant, translation unit, analyzer run, and later function identity rows.

This layer is VERITAS-owned. Third-party tools may provide raw names, USRs, type layouts, and source locations, but VERITAS decides what bytes become stable semantic IDs.

---

# 2. Identity Principles

* IDs are semantic and content-derived where possible.
* Source line numbers are diagnostics, not identity.
* Absolute local machine paths are excluded from semantic hashes.
* Every ID string includes kind and hash algorithm.
* Schema versions and analyzer configs are part of analysis identity.
* Function identity is layered: symbol, variant, body, summary.

ID format:

```text
<kind>:<algorithm>:<hex_digest>
```

Examples:

```text
repo:sha256:...
rev:sha256:...
build:sha256:...
funcsym:sha256:...
funcvar:sha256:...
funcbody:sha256:...
summary:sha256:...
fact:sha256:...
```

---

# 3. Canonicalization Contract

Canonical byte encoding rules:

```text
maps sorted by key
sets sorted by canonical child ID
ordered lists preserved only when order has semantics
strings encoded as UTF-8
default values encoded consistently
timestamps excluded
debug text excluded
local absolute paths rejected unless tagged as external roots
```

M2 must provide one canonicalization library used by all later hashing code. No milestone should hand-roll hashing logic.

---

# 4. Metadata Store

M2 uses SQLite for V1 metadata. SQLite is a logical implementation choice, not a permanent architecture constraint.

Core tables:

```text
repositories
revisions
build_variants
translation_units
analyzer_runs
analysis_configurations
source_anchors
```

Function tables are created in M2 but populated by M4:

```text
function_symbols
function_variants
function_bodies
```

The schema must support fresh database creation and idempotent inserts.

---

# 5. Status and Error Model

M2 uses the project-local M0 status API:

```cpp
veritas::Status
veritas::StatusOr<T>
```

Required status codes:

```text
Ok
InvalidArgument
NotFound
FailedPrecondition
Internal
```

Metadata operations return `Status`, not exceptions, across public interfaces.

---

# 6. API Contract

```cpp
namespace veritas::core {
enum class IdKind;
struct StableId;

StableId MakeStableId(IdKind kind, std::span<const std::byte> canonical_bytes);
std::string ToString(const StableId& id);
StatusOr<StableId> ParseStableId(std::string_view text);
std::vector<std::byte> CanonicalEncode(const CanonicalValue& value);
}
```

```cpp
namespace veritas::summarydb {
class MetadataStore {
 public:
  static veritas::StatusOr<MetadataStore> Open(const std::filesystem::path& db_path);
  veritas::Status ApplySchema();
  veritas::Status PutRepository(const RepositoryRow& row);
  veritas::Status PutRevision(const RevisionRow& row);
  veritas::Status PutBuildVariant(const BuildVariantRow& row);
  veritas::Status PutTranslationUnit(const TranslationUnitRow& row);
  veritas::Status PutAnalyzerRun(const AnalyzerRunRow& row);
};
}
```

---

# 7. Analyzer Identity

Every analyzer run records:

```text
analyzer_name
analyzer_version
schema_version
config_hash
trust_level
```

Analyzer identity is used by:

* summary IDs,
* fact IDs,
* provenance nodes,
* reproducibility checks.

If a config value can change emitted semantic facts, it must be part of `config_hash`.

---

# 8. Function Identity Preparation

M2 defines but does not fully populate these identities:

```text
FunctionSymbolID
FunctionVariantID
FunctionBodyID
```

M4 will provide Clang/LLVM inputs:

```text
mangled_name
canonical_signature
linkage_kind
template_specialization
translation_unit semantic identity
semantic_body_hash
source_anchor
```

M2 must make room for static/internal-linkage identity so unrelated file-local functions do not collide.

---

# 9. Acceptance Tests

Required tests:

```text
same canonical map with reordered keys -> same ID
different ID kind -> different ID string
parse invalid ID string -> InvalidArgument
duplicate repository insert is idempotent
duplicate revision insert is idempotent
metadata schema applies to empty database
absolute untagged path in semantic hash input is rejected
same M1 manifest stored twice creates one logical context
```

---

# 10. Handoff to M3

M3 consumes:

```text
StableId
CanonicalEncode
MetadataStore
repository/revision/build/translation_unit rows
analyzer_run rows
```

M2 is complete when M3 can store immutable summaries using stable IDs and metadata transactions without adding a second hashing or database abstraction.

