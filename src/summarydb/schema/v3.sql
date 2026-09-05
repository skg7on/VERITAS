-- Copyright 2026 VERITAS Contributors
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- v3.sql — fact store and provenance schema (V3), added by M9.
--
-- Persists canonical analysis facts (witness-independent FactID), their
-- per-run occurrence bindings, and the rooted witness DAG that explains each
-- derivation. Facts are immutable once published; a run that re-derives a fact
-- records a new binding and leaves the prior one as history.

INSERT OR IGNORE INTO schema_version (version) VALUES (3);

-- Canonical facts. fact_id is the semantic identity (content-addressed over
-- relation_name + cells, witness-independent); cells is the serialized
-- veritas.fact.v1.Fact message, hex-encoded for the TEXT-only store interface.
CREATE TABLE IF NOT EXISTS analysis_facts (
  fact_id TEXT PRIMARY KEY NOT NULL,
  relation_name TEXT NOT NULL,
  cells_hex TEXT NOT NULL
);

-- Occurrence binding of a fact within one analysis run. Replacing the current
-- fact flips is_current off on the prior binding and inserts a new one, so
-- history stays readable (multiple rows per (run_id, fact_id), distinguished by
-- binding_id). confidence is stored apart from epistemic state.
CREATE TABLE IF NOT EXISTS run_fact_bindings (
  binding_id INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id TEXT NOT NULL,
  fact_id TEXT NOT NULL,
  confidence TEXT,  -- decimal string; '' means "not supplied"
  producer_kind INTEGER NOT NULL,
  analyzer_run_id TEXT,
  scope_kind TEXT,
  scope_id TEXT,
  selected_witness_id TEXT,
  is_current INTEGER NOT NULL,
  FOREIGN KEY (fact_id) REFERENCES analysis_facts(fact_id)
);

-- At most one current binding per (run_id, fact_id); older bindings stay as
-- history with is_current = 0.
CREATE UNIQUE INDEX IF NOT EXISTS run_fact_bindings_current
  ON run_fact_bindings (run_id, fact_id) WHERE is_current = 1;

-- Provenance nodes: one selected witness per derived fact (plus retained
-- alternatives). A fact absent from this table is a rooted input.
CREATE TABLE IF NOT EXISTS provenance_nodes (
  run_id TEXT NOT NULL,
  output_fact_id TEXT NOT NULL,
  witness_id TEXT NOT NULL,
  selected INTEGER NOT NULL,
  producer_kind INTEGER NOT NULL,
  producer_id TEXT,
  rule_id TEXT,
  rule_version TEXT,
  analyzer_run_id TEXT,
  source_anchor_id TEXT,
  summary_id TEXT,
  description TEXT,
  PRIMARY KEY (run_id, output_fact_id, witness_id)
);

-- Provenance edges: the inputs a witness's rule consumed, at their argument
-- position. input_kind is 'rooted' or 'derived'.
CREATE TABLE IF NOT EXISTS provenance_edges (
  run_id TEXT NOT NULL,
  output_fact_id TEXT NOT NULL,
  witness_id TEXT NOT NULL,
  input_kind TEXT NOT NULL,
  input_id TEXT NOT NULL,
  input_ordinal INTEGER NOT NULL,
  PRIMARY KEY (run_id, output_fact_id, witness_id, input_ordinal)
);
