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

#ifndef VERITAS_ANALYSIS_SVF_SVFSESSION_H_
#define VERITAS_ANALYSIS_SVF_SVFSESSION_H_

#include <functional>

#include "analysis/svf/SvfConfig.h"

// Forward declarations of SVF types to keep them out of the public header
namespace SVF {
class SVFIR;
class AndersenWaveDiff;
class SVFG;
}  // namespace SVF

namespace veritas::analysis::pipeline {
class ProgramIr;
}  // namespace veritas::analysis::pipeline

namespace veritas::analysis::svf {

// Status type placeholder - in real implementation would use veritas::Status
class Status {
 public:
  static Status Ok() { return Status(true); }
  static Status Internal(const char* msg) { return Status(false); }
  bool ok() const { return ok_; }
 private:
  explicit Status(bool ok) : ok_(ok) {}
  bool ok_;
};

// SvfSessionView provides callback-scoped access to live SVF analysis results.
// Valid only within the RunWithSvfSession callback.
struct SvfSessionView {
  SVF::SVFIR* svf_ir;
  SVF::AndersenWaveDiff* andersen;
  SVF::SVFG* svfg;
};

// Callback invoked with live SVF session state.
using SvfSessionCallback = std::function<Status(const SvfSessionView&)>;

// RunWithSvfSession builds SVF directly from the live LLVM module owned by
// program_ir, invokes the callback with the constructed SVF state, then
// unconditionally releases all SVF singleton state.
//
// This function is serialized with a process-wide mutex because the pinned
// SVF revision uses global state that is not proven safe for concurrent
// independent contexts.
//
// The callback must not retain pointers to SVF objects after returning.
Status RunWithSvfSession(pipeline::ProgramIr& program_ir,
                         const SvfConfig& config,
                         SvfSessionCallback callback);

// Test-only: verify SVF global state is clean between runs.
#ifdef VERITAS_SVF_SESSION_TEST_HOOKS
bool SvfGlobalStateIsCleanForTest();
#endif

}  // namespace veritas::analysis::svf

#endif  // VERITAS_ANALYSIS_SVF_SVFSESSION_H_
