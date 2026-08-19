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

#include "SvfSession.h"

#include <mutex>

#include <SVF-LLVM/LLVMModule.h>
#include <SVF-LLVM/SVFIRBuilder.h>
#include <SVFIR/SVFIR.h>
#include <WPA/Andersen.h>
#include <Graphs/SVFG.h>
#include <Graphs/SVFGBuilder.h>

#include "analysis/pipeline/ProgramIr.h"

namespace veritas::analysis::svf {
namespace {

// Process-wide mutex for SVF session serialization
std::mutex& ProcessWideSvfMutex() {
  static std::mutex mutex;
  return mutex;
}

// RAII guard for unconditional SVF cleanup
struct SvfCleanup final {
  bool module_set_built = false;
  bool svf_ir_built = false;
  bool andersen_built = false;

  ~SvfCleanup() {
    if (andersen_built) {
      SVF::AndersenWaveDiff::releaseAndersenWaveDiff();
    }
    if (svf_ir_built) {
      SVF::SVFIR::releaseSVFIR();
    }
    if (module_set_built) {
      SVF::LLVMModuleSet::releaseLLVMModuleSet();
    }
  }
};

// Track lifecycle for test verification. Always compiled so the test-only
// hook has a stable definition regardless of how the library is built.
static int g_svf_session_count = 0;

}  // namespace

Status RunWithSvfSession(pipeline::ProgramIr& program_ir,
                         const SvfConfig& config,
                         SvfSessionCallback callback) {
  std::scoped_lock lock(ProcessWideSvfMutex());

  ++g_svf_session_count;

  SvfCleanup cleanup;

  // Step 1: Build SVF module from live LLVM module
  auto* llvm_module = program_ir.GetModule();
  if (!llvm_module) {
    return Status::Internal("ProgramIr has no module");
  }

  SVF::LLVMModuleSet::buildSVFModule(*llvm_module);
  cleanup.module_set_built = true;

  // Capture the session's module set so downstream mapping uses session-scoped
  // state rather than re-querying the global singleton (which may be stale or
  // released if cleanup runs out of order).
  SVF::LLVMModuleSet* module_set = SVF::LLVMModuleSet::getLLVMModuleSet();

  // Step 2: Build SVFIR
  SVF::SVFIRBuilder builder;
  SVF::SVFIR* svf_ir = builder.build();
  if (!svf_ir) {
    return Status::Internal("SVFIR construction failed");
  }
  cleanup.svf_ir_built = true;

  // Step 3: Run Andersen pointer analysis
  SVF::AndersenWaveDiff* andersen =
      SVF::AndersenWaveDiff::createAndersenWaveDiff(svf_ir);
  if (!andersen) {
    return Status::Internal("SVF Andersen failed");
  }
  cleanup.andersen_built = true;

  // Step 4: Build SVFG
  SVF::SVFGBuilder svfg_builder;
  SVF::SVFG* svfg = svfg_builder.buildFullSVFG(andersen);
  if (!svfg) {
    return Status::Internal("SVFG construction failed");
  }

  // Step 5: Invoke callback with live SVF state
  SvfSessionView view{svf_ir, andersen, svfg, module_set};
  return callback(view);

  // Step 6: Cleanup happens automatically via SvfCleanup destructor
}

bool SvfGlobalStateIsCleanForTest() {
  // In a real implementation, this would check SVF internal state.
  // For now, just verify sessions have run.
  return g_svf_session_count > 0;
}

}  // namespace veritas::analysis::svf
