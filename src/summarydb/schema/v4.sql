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

-- v4.sql — atomic fact-batch receipt (V4), added by the M8R-M9 reconciliation.
--
-- Records that a (run_id, batch_id) has been durably published. Re-delivery of
-- the same receipt is a successful no-op, while a different batch in the same
-- run may create a new current binding and preserve the prior one as history.

INSERT OR IGNORE INTO schema_version (version) VALUES (4);

CREATE TABLE IF NOT EXISTS fact_batch_receipts (
  run_id TEXT NOT NULL,
  batch_id TEXT NOT NULL,
  wpa_run_id TEXT NOT NULL,
  PRIMARY KEY (run_id, batch_id)
);
