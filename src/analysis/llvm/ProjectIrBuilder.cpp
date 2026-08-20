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

#include "analysis/llvm/ProjectIrBuilder.h"

#include <memory>
#include <string>
#include <vector>

#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/raw_ostream.h>

#include "analysis/llvm/OriginMap.h"
#include "veritas/build/CompileFlags.h"

namespace veritas::analysis::llvm {
namespace {

namespace fs = std::filesystem;

// Frontend action that emits LLVM IR and captures the generated module before
// the action is destroyed by ClangTool.
class EmitModuleAction : public ::clang::EmitLLVMOnlyAction {
 public:
  EmitModuleAction(::llvm::LLVMContext& context,
                   std::unique_ptr<::llvm::Module>* out)
      : ::clang::EmitLLVMOnlyAction(&context), out_(out) {}

 protected:
  void EndSourceFileAction() override {
    ::clang::EmitLLVMOnlyAction::EndSourceFileAction();
    *out_ = takeModule();
  }

 private:
  std::unique_ptr<::llvm::Module>* out_;
};

class EmitModuleActionFactory
    : public ::clang::tooling::FrontendActionFactory {
 public:
  EmitModuleActionFactory(::llvm::LLVMContext& context,
                          std::unique_ptr<::llvm::Module>* out)
      : context_(context), out_(out) {}

  std::unique_ptr<::clang::FrontendAction> create() override {
    return std::make_unique<EmitModuleAction>(context_, out_);
  }

 private:
  ::llvm::LLVMContext& context_;
  std::unique_ptr<::llvm::Module>* out_;
};

// Generate LLVM IR for one translation unit using the same ClangTool pattern as
// ProjectAstExtractor.
StatusOr<std::unique_ptr<::llvm::Module>> BuildTranslationUnitModule(
    const build::TranslationUnitCommand& command,
    const fs::path& project_root,
    ::llvm::LLVMContext& context) {
  const std::string working_dir =
      (project_root / command.working_directory.relative_path).string();
  const std::string source =
      (project_root / command.source_path.relative_path).string();

  const std::vector<std::string> arguments =
      build::CompileFlags(command, project_root);

  ::clang::tooling::FixedCompilationDatabase database(working_dir, arguments);
  ::clang::tooling::ClangTool tool(database, {source});

  std::unique_ptr<::llvm::Module> module;
  EmitModuleActionFactory factory(context, &module);
  if (tool.run(&factory) != 0) {
    return Status::FailedPrecondition(
        "LLVM IR generation failed for " + command.translation_unit_id);
  }
  if (!module) {
    return Status::FailedPrecondition(
        "Clang produced no module for " + command.translation_unit_id);
  }
  return module;
}

std::string ComputeModuleHash(const ::llvm::Module& module) {
  std::string bytes;
  ::llvm::raw_string_ostream stream(bytes);
  ::llvm::WriteBitcodeToFile(module, stream);
  stream.flush();

  ::llvm::SHA256 hash;
  hash.update(::llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  return ::llvm::toHex(hash.final(), /*LowerCase=*/true);
}

}  // namespace

veritas::StatusOr<pipeline::ProgramIr> ProjectIrBuilder::BuildProjectIr(
    const build::AnalysisManifest& manifest) {
  if (manifest.translation_units.empty()) {
    return Status::FailedPrecondition("manifest has no translation units");
  }

  pipeline::ProgramIr program_ir;
  ::llvm::LLVMContext& context = program_ir.GetContext();

  std::vector<std::unique_ptr<::llvm::Module>> modules;
  modules.reserve(manifest.translation_units.size());

  for (const auto& command : manifest.translation_units) {
    auto module =
        BuildTranslationUnitModule(command, manifest.context.project_root,
                                   context);
    if (!module.ok()) {
      return module.status();
    }
    modules.push_back(std::move(*module));
  }

  // Link every module into the first one.
  auto linked = std::move(modules.front());
  ::llvm::Linker linker(*linked);
  for (std::size_t i = 1; i < modules.size(); ++i) {
    if (linker.linkInModule(std::move(modules[i]))) {
      return Status::FailedPrecondition(
          "failed to link translation unit into whole-program module");
    }
  }

  // Normalize path-dependent fields before deriving any identity so the
  // module hash and origin map are stable across checkout roots and temporary
  // fixture directories (Clang records the source path and a tool-specific
  // module identifier that vary per invocation).
  linked->setModuleIdentifier("veritas.project");
  linked->setSourceFileName("");

  // Populate the origin map: a function's symbol ID is its (mangled) name,
  // qualified with the module identifier when it has internal linkage so two
  // file-local functions in different translation units do not collide.
  OriginMap& origin_map = program_ir.mutable_origin_map();
  for (auto& function : *linked) {
    if (function.isDeclaration()) continue;
    std::string symbol_id = function.getName().str();
    if (function.hasInternalLinkage()) {
      symbol_id = linked->getModuleIdentifier() + "::" + symbol_id;
    }
    origin_map.RecordOrigin(&function, std::move(symbol_id));
  }

  program_ir.SetModuleHash(ComputeModuleHash(*linked));
  program_ir.SetTranslationUnitCount(manifest.translation_units.size());
  program_ir.SetModule(std::move(linked));

  return program_ir;
}

}  // namespace veritas::analysis::llvm
