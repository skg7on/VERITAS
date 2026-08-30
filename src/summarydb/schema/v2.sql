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

-- v2.sql — WPA run-state schema (V2), added by M8R.4 Task 14.
--
-- Tracks production WPA analysis runs, per-component state, and the
-- content-addressed result cache. Facts are not persisted here yet (that is
-- M9); this is the run/component/cache bookkeeping the orchestrator needs for
-- atomic failure and cross-revision reuse.

INSERT OR IGNORE INTO schema_version (version) VALUES (2);

-- One row per WPA analysis run. A run is identified by its content-addressed
-- run_id; a failed or incomplete run keeps its row with a non-success status so
-- a prior successful result remains stale history, never replaced.
CREATE TABLE IF NOT EXISTS wpa_analysis_runs (
  run_id TEXT PRIMARY KEY NOT NULL,
  revision_id TEXT NOT NULL,
  build_variant_id TEXT NOT NULL,
  summary_schema_version TEXT NOT NULL,
  relation_schema_version TEXT NOT NULL,
  rule_bundle_version TEXT NOT NULL,
  model_bundle_version TEXT NOT NULL,
  svf_configuration_hash TEXT NOT NULL,
  wpa_configuration_hash TEXT NOT NULL,
  engine_identity INTEGER NOT NULL,
  engine_toolchain_identity TEXT NOT NULL,
  status INTEGER NOT NULL,
  stale_base_run_id TEXT,
  started_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
  completed_at INTEGER
);

-- One row per component evaluation within a run. The primary key is the run,
-- SCC, and component; a component that failed to evaluate records a non-success
-- status and diagnostics but no replacement result.
CREATE TABLE IF NOT EXISTS wpa_component_states_v2 (
  run_id TEXT NOT NULL,
  scc_id TEXT NOT NULL,
  component_kind INTEGER NOT NULL,
  logical_input_hash TEXT NOT NULL,
  fixpoint_hash TEXT NOT NULL,
  external_hash TEXT NOT NULL,
  result_cache_key TEXT NOT NULL,
  result_object_key TEXT NOT NULL,
  status INTEGER NOT NULL,
  diagnostics TEXT NOT NULL,
  updated_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
  PRIMARY KEY (run_id, scc_id, component_kind),
  FOREIGN KEY (run_id) REFERENCES wpa_analysis_runs(run_id)
);

-- Content-addressed cache of immutable successful component results. A run that
-- reuses an unchanged component records its own state row but points at this
-- shared result object.
CREATE TABLE IF NOT EXISTS wpa_component_result_cache_v2 (
  result_cache_key TEXT PRIMARY KEY NOT NULL,
  logical_input_hash TEXT NOT NULL,
  engine_toolchain_identity TEXT NOT NULL,
  relation_schema_version TEXT NOT NULL,
  rule_bundle_version TEXT NOT NULL,
  model_bundle_version TEXT NOT NULL,
  result_object_key TEXT NOT NULL,
  fixpoint_hash TEXT NOT NULL,
  external_hash TEXT NOT NULL
);
