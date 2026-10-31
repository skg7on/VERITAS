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

#ifndef VERITAS_REVIEW_ASAN_REPLAY_H_
#define VERITAS_REVIEW_ASAN_REPLAY_H_
#include <map>
#include <string>
#include <vector>

#include "veritas/review/StructuralIndex.h"
namespace veritas::review {
// One operator-registered harness. The harness source is trusted code that
// calls the captured project entry point; the payload is the concrete input
// bytes. Neither is ever supplied by a wire request or model output.
struct ReplayHarness {
  std::string source, payload;
};
struct ReplayLimits {
  uint64_t wall_ms = 60000;
  uint64_t output_bytes = 256 * 1024;
  uint64_t rss_bytes = 1024ull * 1024 * 1024;
  uint64_t max_processes = 16;
};
// Trusted replay policy, decoded from the operator backend configuration. An
// empty isolation kind means replay authority is unavailable (UNSUPPORTED).
struct ReplayConfig {
  std::string producer = "asan-replay.v1";
  std::string version = "1";
  std::string compiler;  // absolute trusted compiler driver
  std::string isolation; // "macos-sandbox" | "linux-bwrap" | ""
  std::string work_parent;
  // Paths the enforcing profile must make unreadable (credentials and the live
  // checkout). The producer additionally derives the caller's home directory.
  std::vector<std::string> deny_read_paths;
  // Extra trusted read-only toolchain roots, declared by the operator.
  std::vector<std::string> read_only_paths;
  std::map<std::string, ReplayHarness> harnesses;
  ReplayLimits limits;
};
struct ReplayResult {
  // A closed VerificationResult document (never model-authored).
  Json verification;
  Usage usage;
  // Witness/config bytes for in-process authority admission. They are not part
  // of the wire response; only their content digests are pinned there.
  std::map<std::string, std::string> artifacts;
  int exit_code = -1;
  bool timed_out = false;
  bool compiled = false;
};
// Strict decode of the operator `verification` object. Unknown keys, unsafe
// paths, an empty compiler or nonpositive/capped limits are refused.
StatusOr<ReplayConfig> DecodeReplayConfig(const Json &);
// Admits the captured compiler command for replay. Captured plugin-load,
// response-file, explicit-target and unknown semantic flags are refused before
// any process runs; the analysis target is not assumed to be the replay host.
struct AdmittedReplayCommand {
  std::string source; // captured-relative translation unit
  std::string language;
  std::vector<std::string> flags;
};
StatusOr<AdmittedReplayCommand>
AdmitReplayCompileCommand(const ReviewSnapshot &,
                          const CapturedCompileCommand &,
                          std::string_view input_root);
// One sanitizer stack-frame module location. Darwin renders
// `(module:arch+0xoff)`; ELF renders the arch-less `(module+0xoff)`.
struct SanitizerFrameLocation {
  std::string module, architecture;
  uint64_t offset = 0;
};
// Exposed so tests cover both platform renderings; the producer uses the same
// parser for its decisive site check.
std::vector<SanitizerFrameLocation>
ParseSanitizerFrameLocations(std::string_view report);
// Executes asan-replay.v1 for copy_bound.v1 only. The request must match the
// pinned snapshot and the compiler-established callsite extents. Missing
// compiler/runtime/harness/payload/isolation returns a non-ok status; a
// completed run always returns a VerificationResult, deciding only when the
// sanitizer names the admitted callsite and the declared bound is exceeded.
// `deadline_ms` is the host's remaining aggregate allowance; zero derives a
// total budget from the request and configuration. Every subprocess step is
// bounded by the remainder, so a verify cannot outlive its caller's deadline.
StatusOr<ReplayResult> RunAsanReplay(const Json &request, const ReplayConfig &,
                                     const ReviewSnapshot &,
                                     const StructuralIndex &,
                                     uint64_t deadline_ms = 0);
} // namespace veritas::review
#endif
