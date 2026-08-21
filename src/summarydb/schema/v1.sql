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

-- v1.sql — M2 metadata schema (V1).
--
-- Stores repositories, revisions, build variants, translation units,
-- analyzer runs, and function identity stubs. Function tables are created
-- here but populated by M4.

-- Schema version marker. Used for explicit migration detection.
CREATE TABLE IF NOT EXISTS schema_version (
  version INTEGER PRIMARY KEY
);
INSERT OR IGNORE INTO schema_version (version) VALUES (1);

-- Repositories: one per source tree root.
CREATE TABLE IF NOT EXISTS repositories (
  repository_id TEXT PRIMARY KEY NOT NULL,
  vcs_kind TEXT NOT NULL,
  vcs_revision TEXT NOT NULL,
  source_tree_hash TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

-- Revisions: one per VCS commit or snapshot.
CREATE TABLE IF NOT EXISTS revisions (
  revision_id TEXT PRIMARY KEY NOT NULL,
  repository_id TEXT NOT NULL,
  vcs_revision TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
  FOREIGN KEY (repository_id) REFERENCES repositories(repository_id)
);
CREATE INDEX IF NOT EXISTS idx_revisions_repository ON revisions(repository_id);

-- Build variants: one per unique compiler/target/options combination.
CREATE TABLE IF NOT EXISTS build_variants (
  build_variant_id TEXT PRIMARY KEY NOT NULL,
  target_triple TEXT NOT NULL,
  compiler_id TEXT NOT NULL,
  compiler_version TEXT NOT NULL,
  compile_options_hash TEXT NOT NULL,
  macro_set_hash TEXT NOT NULL,
  include_closure_hash TEXT NOT NULL,
  type_layout_hash TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

-- Translation units: one per source file compilation command.
CREATE TABLE IF NOT EXISTS translation_units (
  translation_unit_id TEXT PRIMARY KEY NOT NULL,
  revision_id TEXT NOT NULL,
  build_variant_id TEXT NOT NULL,
  source_path_root_kind INTEGER NOT NULL,
  source_path_root_id TEXT NOT NULL,
  source_path_relative TEXT NOT NULL,
  working_dir_root_kind INTEGER NOT NULL,
  working_dir_root_id TEXT NOT NULL,
  working_dir_relative TEXT NOT NULL,
  command_hash TEXT NOT NULL,
  preprocessor_hash TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
  FOREIGN KEY (revision_id) REFERENCES revisions(revision_id),
  FOREIGN KEY (build_variant_id) REFERENCES build_variants(build_variant_id)
);
CREATE INDEX IF NOT EXISTS idx_translation_units_revision ON translation_units(revision_id);
CREATE INDEX IF NOT EXISTS idx_translation_units_build_variant ON translation_units(build_variant_id);

-- Analyzer runs: one per analysis invocation with specific config.
CREATE TABLE IF NOT EXISTS analyzer_runs (
  analyzer_run_id INTEGER PRIMARY KEY AUTOINCREMENT,
  analyzer_name TEXT NOT NULL,
  analyzer_version TEXT NOT NULL,
  schema_version INTEGER NOT NULL,
  config_hash TEXT NOT NULL,
  trust_level TEXT NOT NULL,
  started_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

-- Analysis configurations: key-value config pairs for analyzer runs.
CREATE TABLE IF NOT EXISTS analysis_configurations (
  analyzer_run_id INTEGER NOT NULL,
  config_key TEXT NOT NULL,
  config_value TEXT NOT NULL,
  PRIMARY KEY (analyzer_run_id, config_key),
  FOREIGN KEY (analyzer_run_id) REFERENCES analyzer_runs(analyzer_run_id)
);

-- Source anchors: file/line/column locations for diagnostics (not identity).
CREATE TABLE IF NOT EXISTS source_anchors (
  anchor_id INTEGER PRIMARY KEY AUTOINCREMENT,
  translation_unit_id TEXT NOT NULL,
  file_path TEXT NOT NULL,
  start_line INTEGER NOT NULL,
  start_column INTEGER NOT NULL,
  end_line INTEGER NOT NULL,
  end_column INTEGER NOT NULL,
  FOREIGN KEY (translation_unit_id) REFERENCES translation_units(translation_unit_id)
);
CREATE INDEX IF NOT EXISTS idx_source_anchors_translation_unit ON source_anchors(translation_unit_id);

-- Function symbols: one per unique symbol name + linkage + translation unit.
-- Populated by M4, created here for schema completeness.
CREATE TABLE IF NOT EXISTS function_symbols (
  function_symbol_id TEXT PRIMARY KEY NOT NULL,
  translation_unit_id TEXT NOT NULL,
  mangled_name TEXT NOT NULL,
  canonical_signature TEXT NOT NULL,
  linkage_kind TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
  FOREIGN KEY (translation_unit_id) REFERENCES translation_units(translation_unit_id)
);
CREATE INDEX IF NOT EXISTS idx_function_symbols_translation_unit ON function_symbols(translation_unit_id);

-- Function variants: one per template specialization or overload.
-- Populated by M4.
CREATE TABLE IF NOT EXISTS function_variants (
  function_variant_id TEXT PRIMARY KEY NOT NULL,
  function_symbol_id TEXT NOT NULL,
  template_args TEXT,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
  FOREIGN KEY (function_symbol_id) REFERENCES function_symbols(function_symbol_id)
);
CREATE INDEX IF NOT EXISTS idx_function_variants_symbol ON function_variants(function_symbol_id);

-- Function bodies: one per unique semantic body hash.
-- Populated by M4.
CREATE TABLE IF NOT EXISTS function_bodies (
  function_body_id TEXT PRIMARY KEY NOT NULL,
  function_variant_id TEXT NOT NULL,
  semantic_body_hash TEXT NOT NULL,
  source_anchor_id INTEGER,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
  FOREIGN KEY (function_variant_id) REFERENCES function_variants(function_variant_id),
  FOREIGN KEY (source_anchor_id) REFERENCES source_anchors(anchor_id)
);
CREATE INDEX IF NOT EXISTS idx_function_bodies_variant ON function_bodies(function_variant_id);

-- M3: Summary objects — immutable CAS entries for function summaries.
CREATE TABLE IF NOT EXISTS summary_objects (
  summary_id TEXT PRIMARY KEY NOT NULL,
  object_key TEXT NOT NULL UNIQUE,
  schema_version TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

-- M3: Summary components — per-component hashes for incremental invalidation.
CREATE TABLE IF NOT EXISTS summary_components (
  summary_id TEXT NOT NULL,
  component_kind INTEGER NOT NULL,
  semantic_hash TEXT NOT NULL,
  evidence_hash TEXT NOT NULL,
  item_count INTEGER NOT NULL,
  PRIMARY KEY (summary_id, component_kind),
  FOREIGN KEY (summary_id) REFERENCES summary_objects(summary_id)
);

-- M3: Summary bindings — current summary selection per function variant.
CREATE TABLE IF NOT EXISTS summary_bindings (
  function_variant_id TEXT NOT NULL,
  revision_id TEXT NOT NULL,
  build_variant_id TEXT NOT NULL,
  summary_id TEXT NOT NULL,
  publication_epoch INTEGER NOT NULL,
  is_current INTEGER NOT NULL DEFAULT 1,
  PRIMARY KEY (function_variant_id, revision_id, build_variant_id),
  FOREIGN KEY (summary_id) REFERENCES summary_objects(summary_id),
  FOREIGN KEY (revision_id) REFERENCES revisions(revision_id),
  FOREIGN KEY (build_variant_id) REFERENCES build_variants(build_variant_id)
);
CREATE INDEX IF NOT EXISTS idx_summary_bindings_summary ON summary_bindings(summary_id);

-- M6: CPG projections — one immutable graph per (revision, build variant).
CREATE TABLE IF NOT EXISTS cpg_projections (
  projection_id TEXT PRIMARY KEY NOT NULL,
  schema_version TEXT NOT NULL,
  revision_id TEXT NOT NULL,
  build_variant_id TEXT NOT NULL,
  module_hash TEXT NOT NULL,
  summary_ids TEXT NOT NULL,
  canonical_hash TEXT NOT NULL
);

-- M6: CPG nodes — typed, deduplicated per projection.
CREATE TABLE IF NOT EXISTS cpg_nodes (
  projection_id TEXT NOT NULL,
  node_id TEXT NOT NULL,
  node_kind INTEGER NOT NULL,
  node_label TEXT NOT NULL,
  PRIMARY KEY (projection_id, node_id),
  FOREIGN KEY (projection_id) REFERENCES cpg_projections(projection_id)
);

-- M6: CPG edges — adjacency per projection.
CREATE TABLE IF NOT EXISTS cpg_edges (
  projection_id TEXT NOT NULL,
  edge_id TEXT NOT NULL,
  edge_kind INTEGER NOT NULL,
  source_node_id TEXT NOT NULL,
  target_node_id TEXT NOT NULL,
  alias_state INTEGER NOT NULL,
  expandable INTEGER NOT NULL,
  PRIMARY KEY (projection_id, edge_id),
  FOREIGN KEY (projection_id) REFERENCES cpg_projections(projection_id)
);
CREATE INDEX IF NOT EXISTS idx_cpg_edges_source ON cpg_edges(projection_id, source_node_id, edge_kind);
CREATE INDEX IF NOT EXISTS idx_cpg_edges_target ON cpg_edges(projection_id, target_node_id, edge_kind);

-- M6: CPG edge support records (provenance) — ordered per edge.
CREATE TABLE IF NOT EXISTS cpg_edge_support (
  projection_id TEXT NOT NULL,
  edge_id TEXT NOT NULL,
  position INTEGER NOT NULL,
  function_summary_id TEXT NOT NULL,
  provenance_ref TEXT NOT NULL,
  PRIMARY KEY (projection_id, edge_id, position),
  FOREIGN KEY (projection_id, edge_id) REFERENCES cpg_edges(projection_id, edge_id)
);

-- M6: current CPG projection per (revision, build variant).
CREATE TABLE IF NOT EXISTS current_cpg_projections (
  revision_id TEXT NOT NULL,
  build_variant_id TEXT NOT NULL,
  projection_id TEXT NOT NULL,
  PRIMARY KEY (revision_id, build_variant_id)
);

-- M7: Summary dependencies — append-only historical record of every
-- consumer->producer component dependency ever published. Rows are never
-- deleted; they remain explainable after the current index is replaced.
CREATE TABLE IF NOT EXISTS summary_dependencies (
  dependency_id INTEGER PRIMARY KEY AUTOINCREMENT,
  consumer_id TEXT NOT NULL,
  consumer_component INTEGER NOT NULL,
  producer_id TEXT NOT NULL,
  producer_component INTEGER NOT NULL,
  dependency_kind INTEGER NOT NULL,
  sensitivity INTEGER NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);
CREATE INDEX IF NOT EXISTS idx_summary_dependencies_producer
  ON summary_dependencies(producer_id, producer_component);

-- M7: Reverse dependency index — the current (hot) lookup from a producer
-- component to its consumers. Rows are replaced atomically per consumer when
-- a summary republishes its dependencies.
CREATE TABLE IF NOT EXISTS reverse_dependency_index (
  consumer_id TEXT NOT NULL,
  consumer_component INTEGER NOT NULL,
  producer_id TEXT NOT NULL,
  producer_component INTEGER NOT NULL,
  sensitivity INTEGER NOT NULL,
  PRIMARY KEY (consumer_id, consumer_component, producer_id, producer_component)
);
CREATE INDEX IF NOT EXISTS idx_reverse_dependency_producer
  ON reverse_dependency_index(producer_id, producer_component);

-- M7: Summary deltas — immutable record of a semantic/evidence delta between
-- two summary revisions. Populated by the incremental scheduler (M8+); the
-- schema is declared here so M7 tooling can reference it.
CREATE TABLE IF NOT EXISTS summary_deltas (
  delta_id TEXT PRIMARY KEY NOT NULL,
  old_summary_id TEXT NOT NULL,
  new_summary_id TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

-- M7: Component deltas — per-component hashes for each summary delta.
CREATE TABLE IF NOT EXISTS component_deltas (
  delta_id TEXT NOT NULL,
  component_kind INTEGER NOT NULL,
  old_semantic_hash TEXT NOT NULL,
  new_semantic_hash TEXT NOT NULL,
  old_evidence_hash TEXT NOT NULL,
  new_evidence_hash TEXT NOT NULL,
  PRIMARY KEY (delta_id, component_kind),
  FOREIGN KEY (delta_id) REFERENCES summary_deltas(delta_id)
);
