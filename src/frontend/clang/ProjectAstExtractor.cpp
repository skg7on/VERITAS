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

#include "ProjectAstExtractor.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/raw_ostream.h"

#include "SourceAnchorBuilder.h"
#include "veritas/core/Hash.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"

namespace veritas::frontend::clang {

namespace {

struct ClangToolCommand {
  std::string working_directory;
  std::string source_path;
  std::vector<std::string> arguments;
};

StatusOr<ClangToolCommand> ToClangToolCommand(
    const std::filesystem::path& project_root,
    const build::TranslationUnitCommand& command) {
  ClangToolCommand result;

  result.working_directory = (project_root / command.working_directory.relative_path).string();
  result.source_path = (project_root / command.source_path.relative_path).string();
  result.arguments = command.arguments;

  return result;
}

std::string GetLinkageName(::clang::Linkage linkage) {
  switch (linkage) {
    case ::clang::Linkage::Invalid: return "invalid";
    case ::clang::Linkage::None: return "none";
    case ::clang::Linkage::Internal: return "internal";
    case ::clang::Linkage::UniqueExternal: return "unique_external";
    case ::clang::Linkage::VisibleNone: return "visible_none";
    case ::clang::Linkage::Module: return "module";
    case ::clang::Linkage::External: return "external";
    default: return "unknown";
  }
}

std::string MangleFunctionName(const ::clang::FunctionDecl& decl,
                                ::clang::ASTContext& context) {
  auto* mangle_ctx = context.createMangleContext();
  if (!mangle_ctx) {
    return decl.getNameAsString();
  }

  std::string mangled;
  llvm::raw_string_ostream stream(mangled);

  if (const auto* ctor = ::clang::dyn_cast<::clang::CXXConstructorDecl>(&decl)) {
    ::clang::GlobalDecl GD(ctor, ::clang::Ctor_Complete);
    mangle_ctx->mangleName(GD, stream);
  } else if (const auto* dtor = ::clang::dyn_cast<::clang::CXXDestructorDecl>(&decl)) {
    ::clang::GlobalDecl GD(dtor, ::clang::Dtor_Complete);
    mangle_ctx->mangleName(GD, stream);
  } else if (mangle_ctx->shouldMangleDeclName(&decl)) {
    ::clang::GlobalDecl GD(&decl);
    mangle_ctx->mangleName(GD, stream);
  } else {
    delete mangle_ctx;
    return decl.getNameAsString();
  }

  stream.flush();
  delete mangle_ctx;
  return mangled;
}

std::string GetCanonicalSignature(const ::clang::FunctionDecl& decl) {
  std::string sig = decl.getReturnType().getAsString();
  sig += " ";
  sig += decl.getQualifiedNameAsString();
  sig += "(";

  bool first = true;
  for (const auto* param : decl.parameters()) {
    if (!first) sig += ", ";
    sig += param->getType().getAsString();
    first = false;
  }

  sig += ")";

  if (const auto* method = ::clang::dyn_cast<::clang::CXXMethodDecl>(&decl)) {
    if (method->isConst()) {
      sig += " const";
    }
  }

  return sig;
}

std::string GetTemplateIdentity(const ::clang::FunctionDecl& decl) {
  if (const auto* tmpl = decl.getPrimaryTemplate()) {
    std::string result = "template<";
    result += tmpl->getNameAsString();
    result += ">";
    return result;
  }

  if (const auto* spec = ::clang::dyn_cast<::clang::FunctionTemplateSpecializationInfo>(
          decl.getTemplateSpecializationInfo())) {
    return "specialization";
  }

  return "";
}

core::StableId BuildFunctionSymbolId(const ::clang::FunctionDecl& decl,
                                      const std::string& mangled_name,
                                      const core::StableId& translation_unit_id) {
  std::string canonical_form;
  canonical_form += decl.getQualifiedNameAsString();
  canonical_form += "|";
  canonical_form += mangled_name;
  canonical_form += "|";
  canonical_form += GetLinkageName(decl.getLinkageInternal());

  if (decl.getLinkageInternal() == ::clang::Linkage::Internal) {
    canonical_form += "|";
    canonical_form += translation_unit_id.digest_hex;
  }

  // Convert string to bytes for hashing
  std::vector<std::byte> bytes;
  bytes.reserve(canonical_form.size());
  for (char c : canonical_form) {
    bytes.push_back(static_cast<std::byte>(c));
  }

  auto digest = core::ComputeSHA256(bytes);
  std::string hex = core::DigestToHex(digest);

  return core::MakeStableId(core::IdKind::kFunctionSymbol,
                            std::as_bytes(std::span(digest)));
}

class FunctionDeclVisitor : public ::clang::RecursiveASTVisitor<FunctionDeclVisitor> {
 public:
  FunctionDeclVisitor(::clang::ASTContext& context,
                       const core::StableId& translation_unit_id,
                       ProjectAstIndex* output)
      : context_(context),
        translation_unit_id_(translation_unit_id),
        output_(output) {}

  bool VisitFunctionDecl(::clang::FunctionDecl* decl) {
    if (!decl || !decl->hasBody()) {
      return true;
    }

    const auto* canonical = decl->getCanonicalDecl();
    if (!canonical) {
      canonical = decl;
    }

    SourceAnchorBuilder anchor_builder;

    std::string mangled = MangleFunctionName(*canonical, context_);

    ExtractedFunctionDecl extracted;
    extracted.translation_unit_id = translation_unit_id_;
    extracted.qualified_name = canonical->getQualifiedNameAsString();
    extracted.mangled_name = mangled;
    extracted.canonical_signature = GetCanonicalSignature(*canonical);
    extracted.linkage_kind = GetLinkageName(canonical->getLinkageInternal());
    extracted.template_identity = GetTemplateIdentity(*canonical);
    extracted.source_anchor = anchor_builder.Build(
        canonical->getSourceRange(), context_.getSourceManager());
    extracted.function_symbol_id = BuildFunctionSymbolId(
        *canonical, mangled, translation_unit_id_);

    output_->declarations.push_back(std::move(extracted));

    return true;
  }

 private:
  ::clang::ASTContext& context_;
  core::StableId translation_unit_id_;
  ProjectAstIndex* output_;
};

class FunctionDeclConsumer : public ::clang::ASTConsumer {
 public:
  FunctionDeclConsumer(::clang::ASTContext& context,
                        const core::StableId& translation_unit_id,
                        ProjectAstIndex* output)
      : visitor_(context, translation_unit_id, output) {}

  void HandleTranslationUnit(::clang::ASTContext& context) override {
    visitor_.TraverseDecl(context.getTranslationUnitDecl());
  }

 private:
  FunctionDeclVisitor visitor_;
};

class ProjectAstAction : public ::clang::ASTFrontendAction {
 public:
  ProjectAstAction(const core::StableId& translation_unit_id,
                    ProjectAstIndex* output)
      : translation_unit_id_(translation_unit_id), output_(output) {}

  std::unique_ptr<::clang::ASTConsumer> CreateASTConsumer(
      ::clang::CompilerInstance& compiler, llvm::StringRef /*file*/) override {
    return std::make_unique<FunctionDeclConsumer>(
        compiler.getASTContext(), translation_unit_id_, output_);
  }

 private:
  core::StableId translation_unit_id_;
  ProjectAstIndex* output_;
};

class ProjectAstActionFactory : public ::clang::tooling::FrontendActionFactory {
 public:
  ProjectAstActionFactory(const core::StableId& translation_unit_id,
                           ProjectAstIndex* output)
      : translation_unit_id_(translation_unit_id), output_(output) {}

  std::unique_ptr<::clang::FrontendAction> create() override {
    return std::make_unique<ProjectAstAction>(translation_unit_id_, output_);
  }

 private:
  core::StableId translation_unit_id_;
  ProjectAstIndex* output_;
};

}  // namespace

const ExtractedFunctionDecl* ProjectAstIndex::FindByMangledName(
    std::string_view mangled_name) const {
  for (const auto& decl : declarations) {
    if (decl.mangled_name == mangled_name) {
      return &decl;
    }
  }
  return nullptr;
}

StatusOr<ProjectAstIndex> ProjectAstExtractor::ExtractProject(
    const build::AnalysisManifest& manifest) {
  ProjectAstIndex index;

  for (const auto& command : manifest.translation_units) {
    auto invocation = ToClangToolCommand(manifest.context.project_root, command);
    if (!invocation.ok()) {
      return Status::FailedPrecondition(
                    "Failed to convert translation unit command: " +
                    std::string(invocation.status().message()));
    }

    ::clang::tooling::FixedCompilationDatabase database(
        invocation->working_directory, invocation->arguments);
    ::clang::tooling::ClangTool tool(database, {invocation->source_path});

    auto tu_id = core::ParseStableId(command.translation_unit_id);
    if (!tu_id.ok()) {
      return Status::FailedPrecondition(
                    "Invalid translation unit ID: " + command.translation_unit_id);
    }

    auto action_factory = std::make_unique<ProjectAstActionFactory>(
        tu_id.value(), &index);

    if (tool.run(action_factory.get()) != 0) {
      return Status::FailedPrecondition(
                    "Clang AST extraction failed for " + command.translation_unit_id);
    }

    ++index.processed_translation_units;
  }

  return index;
}

}  // namespace veritas::frontend::clang
