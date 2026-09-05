// Copyright 2026 VERITAS Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// FactProto.h — bidirectional conversion between the canonical AnalysisFact
// and its versioned wire form (veritas.fact.v1.Fact).
//
// The wire form is the durable serialization used by the M9 fact store: it
// carries the fact ID, the relation name, and the typed semantic cells. Fact
// identity stays witness-independent because the ID is re-derived from the
// semantic row on both directions, so a mismatched ID is rejected rather than
// trusted.

#ifndef VERITAS_FACTS_FACT_PROTO_H_
#define VERITAS_FACTS_FACT_PROTO_H_

#include "veritas/core/Status.h"
#include "veritas/fact/v1/fact.pb.h"
#include "veritas/facts/AnalysisFact.h"

namespace veritas::facts {

namespace fact_proto = veritas::fact::v1;

// Serializes a canonical fact. Rejects a fact whose fact_id does not match its
// semantic row (re-derived via MakeFact).
StatusOr<fact_proto::Fact> ToProtoFact(const AnalysisFact& fact);

// Reconstructs a canonical fact. Rejects a malformed fact_id, an unknown
// relation name, a cell that does not match its relation schema, or a fact_id
// that does not match the reconstructed semantic row.
StatusOr<AnalysisFact> FromProtoFact(const fact_proto::Fact& proto);

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_FACT_PROTO_H_
