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

#include "veritas/review/StructuralIndex.h"
#include "StructuralIndexInternal.h"
#include "veritas/review/ProgramScope.h"

#include <algorithm>
#include <chrono>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/Builtins.h>
#include <clang/Frontend/ASTUnit.h>
#include <clang/Lex/Lexer.h>
#include <clang/Tooling/Tooling.h>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/StringSaver.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <poll.h>
#include <set>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

extern char **environ;
namespace veritas::review {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::string_view kRoot = "/veritas-input/";
Json Strings(const std::vector<std::string> &strings) {
  Json::Array a;
  for (const auto &s : strings)
    a.push_back(s);
  return a;
}
void Gap(Coverage &coverage, std::string ref, std::string reason) {
  coverage.supported = false;
  coverage.traversal = "INCOMPLETE";
  coverage.frontier_refs.push_back(std::move(ref));
  coverage.reasons.push_back(std::move(reason));
}
struct Command {
  std::string file;
  std::vector<std::string> args;
};
Status Refuse(std::string_view detail) {
  return Status::FailedPrecondition("unsupported captured compiler command: " +
                                    std::string(detail));
}
Status Expand(const ReviewSnapshot &snapshot,
              const CapturedCompileCommand &command,
              const std::vector<std::string> &input,
              std::vector<std::string> &output, uint64_t &bytes,
              unsigned depth = 0) {
  if (depth > 4)
    return Refuse("response-file nesting/cycle limit");
  for (const auto &arg : input) {
    if (arg.empty() || arg.find('\0') != std::string::npos)
      return Refuse("empty/NUL argument");
    if (arg.starts_with('@')) {
      auto path = snapshot.CompilerPath(command.directory, arg.substr(1));
      if (!path.ok())
        return Refuse("response path outside snapshot");
      auto text = snapshot.Read(*path);
      if (!text.ok() || text->size() > 65536 - bytes)
        return Refuse("response file missing or too large");
      bytes += text->size();
      if (text->find('\0') != std::string::npos)
        return Refuse("NUL response file");
      llvm::BumpPtrAllocator alloc;
      llvm::StringSaver saver(alloc);
      llvm::SmallVector<const char *, 32> tokens;
      llvm::cl::TokenizeGNUCommandLine(*text, saver, tokens);
      std::vector<std::string> expanded;
      for (const auto *token : tokens)
        expanded.emplace_back(token);
      auto status =
          Expand(snapshot, command, expanded, output, bytes, depth + 1);
      if (!status.ok())
        return status;
    } else {
      if (output.size() >= 4096 || arg.size() > 65536)
        return Refuse("argument limit");
      output.push_back(arg);
    }
  }
  return Status::Ok();
}
StatusOr<Command> Admit(const ReviewSnapshot &snapshot,
                        const CapturedCompileCommand &command) {
  if (command.arguments.empty())
    return Refuse("missing compiler");
  // The project executable is never run. Only recognized Clang driver syntax is
  // interpreted by the linked, versioned compiler library.
  const auto driver =
      std::filesystem::path(command.arguments[0]).filename().string();
  if (driver != "clang" && driver != "clang++")
    return Refuse("compiler/wrapper");
  std::vector<std::string> input(command.arguments.begin() + 1,
                                 command.arguments.end()),
      args;
  uint64_t response_bytes = 0;
  auto status = Expand(snapshot, command, input, args, response_bytes);
  if (!status.ok())
    return status;
  Command out{command.file,
              {"--target=x86_64-unknown-linux-gnu", "-nostdinc", "-nostdinc++",
               "-nobuiltininc"}};
  const auto extension =
      std::filesystem::path(command.file).extension().string();
  const std::set<std::string> extensions = {
      ".c", ".cc", ".cpp", ".cxx", ".C", ".c++", ".h", ".hh", ".hpp", ".hxx"};
  if (!extensions.contains(extension))
    return Refuse("unsupported source language");
  out.args.insert(
      out.args.end(),
      {"-x", driver == "clang" && (extension == ".c" || extension == ".h")
                 ? "c"
                 : "c++"});
  bool source_seen = false;
  const std::set<std::string> switches = {
      "-c",        "-g",           "-g0",       "-O0",      "-O1",
      "-O2",       "-O3",          "-Os",       "-Oz",      "-Wall",
      "-Wextra",   "-Werror",      "-pedantic", "-pthread", "-fno-exceptions",
      "-fno-rtti", "-fexceptions", "-frtti",    "-fPIC",    "-fpic",
      "-fPIE"};
  const std::set<std::string> standards = {
      "c11",   "c17",   "c23",   "gnu11",   "gnu17",   "gnu23",   "c++14",
      "c++17", "c++20", "c++23", "gnu++14", "gnu++17", "gnu++20", "gnu++23"};
  for (size_t i = 0; i < args.size(); ++i) {
    auto arg = args[i];
    auto value = [&]() -> StatusOr<std::string> {
      if (++i == args.size() || args[i].empty() || args[i].starts_with('-'))
        return Refuse("missing flag operand");
      return args[i];
    };
    if (switches.contains(arg)) {
      if (arg != "-c")
        out.args.push_back(arg);
      continue;
    }
    if (arg == "-o" || arg == "-MF" || arg == "-MT" || arg == "-MQ") {
      auto v = value();
      if (!v.ok())
        return v.status();
      continue; // syntax-only: no outputs
    }
    if (arg == "-MD" || arg == "-MMD" || arg == "-MP")
      continue;
    if (arg.starts_with("-std=")) {
      if (!standards.contains(arg.substr(5)))
        return Refuse("language standard");
      out.args.push_back(arg);
      continue;
    }
    if (arg == "--target" || arg == "-target" || arg.starts_with("--target=")) {
      auto v = arg.starts_with("--target=")
                   ? StatusOr<std::string>(arg.substr(9))
                   : value();
      if (!v.ok() || *v != "x86_64-unknown-linux-gnu")
        return Refuse("ABI/target");
      continue;
    }
    if (arg == "-x") {
      auto v = value();
      if (!v.ok() || (*v != "c" && *v != "c++"))
        return Refuse("language");
      out.args.insert(out.args.end(), {arg, *v});
      continue;
    }
    if (arg == "-D" || arg == "-U" || arg.starts_with("-D") ||
        arg.starts_with("-U")) {
      auto v = arg.size() == 2 ? value() : StatusOr<std::string>(arg.substr(2));
      if (!v.ok() || v->empty() ||
          v->find_first_of("\n\r") != std::string::npos)
        return Refuse("macro operand");
      out.args.push_back(arg.substr(0, 2) + *v);
      continue;
    }
    bool directory = arg == "-I" || arg == "-isystem" || arg == "-iquote" ||
                     (arg.starts_with("-I") && arg.size() > 2);
    if (directory || arg == "-include" || arg == "-imacros") {
      auto flag = arg.starts_with("-I") ? "-I" : arg;
      auto v = arg.starts_with("-I") && arg.size() > 2
                   ? StatusOr<std::string>(arg.substr(2))
                   : value();
      if (!v.ok())
        return v.status();
      auto path = snapshot.CompilerPath(command.directory, *v);
      if (!path.ok())
        return Refuse("include path outside snapshot");
      bool found = false;
      for (const auto &p : snapshot.captured_paths())
        if (directory ? (*path == "." || p.starts_with(*path + "/"))
                      : p == *path)
          found = true;
      if (!found)
        return Refuse("uncaptured include input");
      out.args.insert(out.args.end(),
                      {flag, std::string(kRoot) + (*path == "." ? "" : *path)});
      continue;
    }
    if (arg.starts_with('-') || arg.starts_with('@'))
      return Refuse(arg);
    auto path = snapshot.CompilerPath(command.directory, arg);
    if (!path.ok() || *path != command.file || source_seen)
      return Refuse("unexpected positional input");
    source_seen = true;
  }
  if (!source_seen)
    return Refuse("missing source operand");
  return out;
}

class Diagnostics : public clang::DiagnosticConsumer {
public:
  bool failed = false;
  void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                        const clang::Diagnostic &info) override {
    clang::DiagnosticConsumer::HandleDiagnostic(level, info);
    if (level >= clang::DiagnosticsEngine::Error)
      failed = true;
  }
};
// This producer deliberately admits less than general C/C++ semantics. It uses
// compiler layout and AST identity; it never guesses a pointee extent or proves
// feasibility. Guard admission is limited to unsigned unchanged parameter sizes
// and structured, loop/goto-free branches.
class Collector : public clang::RecursiveASTVisitor<Collector> {
public:
  Collector(clang::ASTContext &context,
            const std::map<std::string, ContextId> &contexts,
            Json::Array &entities, Json::Array &copies, uint64_t limit)
      : ctx_(context), contexts_(contexts), entities_(entities),
        copies_(copies), limit_(limit) {}
  bool truncated = false;
  bool TraverseDecl(clang::Decl *decl) {
    if (entities_.size() >= limit_) {
      truncated = true;
      return false;
    }
    return clang::RecursiveASTVisitor<Collector>::TraverseDecl(decl);
  }
  bool TraverseFunctionDecl(clang::FunctionDecl *decl) {
    auto *old = function_;
    function_ = decl;
    bool result =
        clang::RecursiveASTVisitor<Collector>::TraverseFunctionDecl(decl);
    function_ = old;
    return result;
  }
  bool VisitFunctionDecl(clang::FunctionDecl *decl) {
    if (decl->isThisDeclarationADefinition())
      Add("FUNCTION", decl->getQualifiedNameAsString(),
          decl->getType().getAsString(), decl->getSourceRange());
    return !truncated;
  }
  bool VisitVarDecl(clang::VarDecl *decl) {
    if (decl->hasLocalStorage() && ctx_.getAsConstantArrayType(decl->getType()))
      Add("MEMORY", decl->getNameAsString(), decl->getType().getAsString(),
          decl->getSourceRange());
    return !truncated;
  }
  bool VisitCallExpr(clang::CallExpr *call) {
    auto *callee = call->getDirectCallee();
    auto ordinal = Add(
        "CALLSITE", callee ? callee->getQualifiedNameAsString() : "<indirect>",
        call->getType().getAsString(), call->getSourceRange());
    if (!ordinal)
      return !truncated;
    auto &object = std::get<Json::Object>(entities_[*ordinal].data);
    object["direct_call"] = callee != nullptr;
    Json::Array operands;
    std::vector<uint32_t> ordinals;
    for (unsigned i = 0; i < call->getNumArgs(); ++i) {
      auto *expr = call->getArg(i);
      auto operand = Add("VALUE", "operand", expr->getType().getAsString(),
                         expr->getSourceRange(), i);
      if (!operand)
        break;
      operands.push_back(*operand);
      ordinals.push_back(*operand);
    }
    // Re-fetch after Add: vector growth may invalidate references.
    std::get<Json::Object>(entities_[*ordinal].data)["operands"] =
        std::move(operands);
    Json::Array reasons, guards;
    bool admitted =
        callee && !callee->getDefinition() && call->getNumArgs() == 3 &&
        (callee->getBuiltinID() == clang::Builtin::BImemcpy ||
         callee->getBuiltinID() == clang::Builtin::BI__builtin_memcpy);
    if (!admitted)
      reasons.push_back("unregistered or indirect copy form");
    if (call->getBeginLoc().isMacroID() || call->getEndLoc().isMacroID()) {
      admitted = false;
      reasons.push_back("macro-expanded copy is unsupported");
    }
    if (callee) {
      const auto *proto = callee->getType()->getAs<clang::FunctionProtoType>();
      if (!proto || proto->getCallConv() != clang::CC_C ||
          callee->hasAttr<clang::AsmLabelAttr>()) {
        admitted = false;
        reasons.push_back("copy ABI or assembler binding is unsupported");
      }
    }
    if (ordinals.size() != 3)
      admitted = false;
    const clang::VarDecl *array = nullptr;
    const clang::ValueDecl *length = nullptr;
    std::optional<uint64_t> extent;
    if (admitted) {
      for (unsigned i = 0; i < 3; ++i)
        Scan(call->getArg(i), [&](const clang::Stmt *stmt) {
          if (llvm::isa<clang::ExplicitCastExpr>(stmt) ||
              stmt->getBeginLoc().isMacroID() ||
              stmt->getEndLoc().isMacroID()) {
            admitted = false;
          }
        });
      if (!admitted)
        reasons.push_back("explicit cast or macro copy operand is unsupported");
    }
    if (admitted) {
      auto *dest = llvm::dyn_cast<clang::DeclRefExpr>(
          call->getArg(0)->IgnoreParenImpCasts());
      if (dest)
        array = llvm::dyn_cast<clang::VarDecl>(dest->getDecl());
      const auto *type =
          array ? ctx_.getAsConstantArrayType(array->getType()) : nullptr;
      if (array && array->hasLocalStorage() && type &&
          type->getElementType()->isIntegerType() &&
          !array->getType().isVolatileQualified())
        extent = ctx_.getTypeSizeInChars(array->getType()).getQuantity();
      else
        reasons.push_back(
            "destination is not a direct local constant integer array");
      auto *size = llvm::dyn_cast<clang::DeclRefExpr>(
          call->getArg(2)->IgnoreParenImpCasts());
      if (size && size->getDecl()->getType()->isUnsignedIntegerType() &&
          llvm::isa<clang::ParmVarDecl>(size->getDecl()))
        length = size->getDecl();
      if (extent && length && function_ && function_->hasBody()) {
        bool unchanged = true, structured = true;
        Scan(function_->getBody(), [&](const clang::Stmt *stmt) {
          if (llvm::isa<clang::GotoStmt, clang::IndirectGotoStmt,
                        clang::ForStmt, clang::WhileStmt, clang::DoStmt,
                        clang::SwitchStmt, clang::LambdaExpr, clang::AsmStmt>(
                  stmt))
            structured = false;
          if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(stmt))
            if (binary->isAssignmentOp() &&
                References(binary->getLHS(), length))
              unchanged = false;
          if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(stmt))
            if ((unary->isIncrementDecrementOp() ||
                 unary->getOpcode() == clang::UO_AddrOf) &&
                References(unary->getSubExpr(), length))
              unchanged = false;
          if (const auto *other = llvm::dyn_cast<clang::CallExpr>(stmt)) {
            const auto *target = other->getDirectCallee();
            for (unsigned i = 0; i < other->getNumArgs(); ++i)
              if (References(other->getArg(i), length) &&
                  (!target || i >= target->getNumParams() ||
                   target->getParamDecl(i)->getType()->isReferenceType()))
                unchanged = false;
          }
          if ((llvm::isa<clang::CXXConstructExpr>(stmt) ||
               llvm::isa<clang::InitListExpr>(stmt)) &&
              References(stmt, length))
            unchanged = false;
          if (const auto *declstmt = llvm::dyn_cast<clang::DeclStmt>(stmt))
            for (const auto *decl : declstmt->decls())
              if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl))
                if (var->hasInit() &&
                    (var->getType()->isReferenceType() ||
                     !var->getType()->isScalarType()) &&
                    References(var->getInit(), length))
                  unchanged = false;
        });
        if (unchanged && structured) {
          Scan(function_->getBody(), [&](const clang::Stmt *stmt) {
            const auto *branch = llvm::dyn_cast<clang::IfStmt>(stmt);
            if (!branch || branch->getCond()->getBeginLoc().isMacroID())
              return;
            const auto *comparison = llvm::dyn_cast<clang::BinaryOperator>(
                branch->getCond()->IgnoreParens());
            if (!comparison || !Refers(comparison->getLHS(), length))
              return;
            const auto *bound = comparison->getRHS()->IgnoreParenImpCasts();
            if (!llvm::isa<clang::IntegerLiteral,
                           clang::UnaryExprOrTypeTraitExpr>(bound))
              return;
            if (const auto *trait =
                    llvm::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(bound))
              if (trait->getKind() != clang::UETT_SizeOf ||
                  trait->isArgumentType() ||
                  !Refers(trait->getArgumentExpr(), array))
                return;
            clang::Expr::EvalResult evaluated;
            if (!bound->EvaluateAsInt(evaluated, ctx_) ||
                evaluated.Val.getInt().isNegative() ||
                evaluated.Val.getInt().getLimitedValue() != *extent)
              return;
            bool le = comparison->getOpcode() == clang::BO_LE;
            bool gt = comparison->getOpcode() == clang::BO_GT;
            if (!le && !gt)
              return;
            bool dominates = (le && Contains(branch->getThen(), call)) ||
                             (gt && Contains(branch->getElse(), call));
            auto condition =
                Add("VALUE", "size guard", comparison->getType().getAsString(),
                    comparison->getSourceRange());
            if (condition)
              guards.push_back(Json::Object{{"condition", *condition},
                                            {"dominates", dominates}});
          });
        } else
          reasons.push_back(
              "guard size is mutated/aliased or control flow is unsupported");
      }
      if (guards.empty())
        reasons.push_back("no admitted local size guard");
    }
    copies_.push_back(
        Json::Object{{"callsite", *ordinal},
                     {"admitted", admitted},
                     {"extent", extent ? Json(*extent) : Json(nullptr)},
                     {"guards", std::move(guards)},
                     {"reasons", std::move(reasons)}});
    return !truncated;
  }

private:
  clang::ASTContext &ctx_;
  const std::map<std::string, ContextId> &contexts_;
  Json::Array &entities_, &copies_;
  uint64_t limit_;
  clang::FunctionDecl *function_ = nullptr;
  template <class F> static void Scan(const clang::Stmt *stmt, const F &f) {
    if (!stmt)
      return;
    f(stmt);
    for (const auto *child : stmt->children())
      Scan(child, f);
  }
  static bool References(const clang::Stmt *stmt,
                         const clang::ValueDecl *decl) {
    bool found = false;
    Scan(stmt, [&](const clang::Stmt *part) {
      if (const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(part))
        if (ref->getDecl() == decl)
          found = true;
    });
    return found;
  }
  static bool Refers(const clang::Expr *expr, const clang::ValueDecl *decl) {
    if (!expr)
      return false;
    const auto *ref =
        llvm::dyn_cast<clang::DeclRefExpr>(expr->IgnoreParenImpCasts());
    return ref && ref->getDecl() == decl;
  }
  static bool Contains(const clang::Stmt *container,
                       const clang::Stmt *needle) {
    bool found = false;
    Scan(container, [&](const clang::Stmt *stmt) {
      if (stmt == needle)
        found = true;
    });
    return found;
  }
  std::optional<uint32_t> Add(std::string kind, std::string name,
                              std::string type, clang::SourceRange range,
                              std::optional<unsigned> operand = {}) {
    if (entities_.size() >= limit_) {
      truncated = true;
      return {};
    }
    auto &sm = ctx_.getSourceManager();
    auto begin = sm.getExpansionLoc(range.getBegin());
    auto end = clang::Lexer::getLocForEndOfToken(
        sm.getExpansionLoc(range.getEnd()), 0, sm, ctx_.getLangOpts());
    if (!begin.isValid() || !end.isValid() ||
        sm.getFileID(begin) != sm.getFileID(end))
      return {};
    auto path = sm.getFilename(begin).str();
    if (!path.starts_with(kRoot))
      return {};
    path = path.substr(kRoot.size());
    auto it = contexts_.find(path);
    if (it == contexts_.end())
      return {};
    auto ordinal = static_cast<uint32_t>(entities_.size());
    entities_.push_back(Json::Object{
        {"ordinal", ordinal},
        {"kind", std::move(kind)},
        {"name", std::move(name)},
        {"type", std::move(type)},
        {"path", path},
        {"start", sm.getFileOffset(begin)},
        {"end", sm.getFileOffset(end)},
        {"source_ref", it->second.value()},
        {"operand_index", operand ? Json(*operand) : Json(nullptr)},
        {"direct_call", false},
        {"operands", Json::Array{}}});
    return ordinal;
  }
};
uint64_t Rss(pid_t pid) {
#ifdef __APPLE__
  proc_taskinfo task{};
  if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task, sizeof(task)) ==
      sizeof(task))
    return task.pti_resident_size;
#else
  std::ifstream input("/proc/" + std::to_string(pid) + "/statm");
  uint64_t pages = 0, resident = 0;
  if (input >> pages >> resident)
    return resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#endif
  return 0;
}
uint64_t RusageRss(const rusage &usage) {
#ifdef __APPLE__
  return usage.ru_maxrss;
#else
  return static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#endif
}
struct WorkerResult {
  Json document;
  uint64_t rss, cpu_ms, bytes;
};
StatusOr<WorkerResult>
ParseWorker(const std::vector<Command> &commands,
            const std::map<std::string, std::string> &files,
            const std::map<std::string, ContextId> &contexts,
            const StructuralBudget &budget, Clock::time_point start) {
  int pipefd[2];
  if (pipe(pipefd) != 0)
    return Status::Internal("compiler pipe failed");
  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return Status::Internal("compiler fork failed");
  }
  if (pid == 0) {
    close(pipefd[0]);
    setpgid(0, 0);
    char path[] = "PATH=/usr/bin:/bin", locale[] = "LANG=C",
         home[] = "HOME=/nonexistent", epoch[] = "SOURCE_DATE_EPOCH=0";
    char *env[] = {path, locale, home, epoch, nullptr};
    environ = env;
    rlimit core{0, 0};
    setrlimit(RLIMIT_CORE, &core);
    auto fs = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
    fs->setCurrentWorkingDirectory(std::string(kRoot));
    for (const auto &[name, bytes] : files)
      fs->addFile(std::string(kRoot) + name, 0,
                  llvm::MemoryBuffer::getMemBufferCopy(bytes));
    Json::Array entities, copies, frontier;
    uint64_t invocations = 0;
    for (const auto &command : commands) {
      auto it = files.find(command.file);
      if (it == files.end()) {
        frontier.push_back("uncaptured-source:" + command.file);
        continue;
      }
      Diagnostics diagnostics;
      ++invocations;
      auto ast = clang::tooling::buildASTFromCodeWithArgs(
          it->second, command.args, std::string(kRoot) + command.file,
          "clang++", std::make_shared<clang::PCHContainerOperations>(),
          clang::tooling::getClangStripDependencyFileAdjuster(), {},
          &diagnostics, fs);
      if (!ast || diagnostics.failed ||
          ast->getDiagnostics().hasErrorOccurred()) {
        // Recovery ASTs do not establish operand/layout equivalence.
        frontier.push_back("compiler-error:" + command.file);
        continue;
      }
      Collector visitor(ast->getASTContext(), contexts, entities, copies,
                        budget.max_entities);
      visitor.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
      if (visitor.truncated) {
        frontier.push_back("entity-budget:" + command.file);
        break;
      }
    }
    auto output = CanonicalJson(Json::Object{{"entities", std::move(entities)},
                                             {"copies", std::move(copies)},
                                             {"frontier", std::move(frontier)},
                                             {"invocations", invocations}});
    if (!output.ok() || output->size() > budget.output_bytes)
      _exit(2);
    size_t offset = 0;
    while (offset < output->size()) {
      auto n =
          write(pipefd[1], output->data() + offset, output->size() - offset);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        _exit(3);
      offset += n;
    }
    close(pipefd[1]);
    _exit(0);
  }
  close(pipefd[1]);
  setpgid(pid, pid);
  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
  std::string output;
  uint64_t peak = 0;
  bool eof = false, reaped = false, exceeded = false;
  int status = 0;
  rusage usage{};
  while (!eof || !reaped) {
    char buffer[8192];
    auto n = read(pipefd[0], buffer, sizeof(buffer));
    if (n > 0) {
      if (static_cast<uint64_t>(n) > budget.output_bytes - output.size()) {
        exceeded = true;
        break;
      }
      output.append(buffer, n);
    } else if (n == 0)
      eof = true;
    else if (errno != EAGAIN && errno != EINTR) {
      exceeded = true;
      break;
    }
    peak = std::max(peak, Rss(pid));
    if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                              start)
                .count() >= static_cast<int64_t>(budget.wall_ms) ||
        peak > budget.child_rss_bytes) {
      exceeded = true;
      break;
    }
    if (!reaped) {
      auto w = wait4(pid, &status, WNOHANG, &usage);
      if (w == pid)
        reaped = true;
      else if (w < 0 && errno != EINTR) {
        exceeded = true;
        break;
      }
    }
    if (n <= 0 && !eof) {
      pollfd poller{pipefd[0], POLLIN | POLLHUP, 0};
      poll(&poller, 1, 25);
    }
  }
  if (exceeded)
    kill(-pid, SIGKILL);
  if (!reaped)
    while (wait4(pid, &status, 0, &usage) < 0 && errno == EINTR) {
    }
  close(pipefd[0]);
  peak = std::max(peak, RusageRss(usage));
  if (exceeded || peak > budget.child_rss_bytes || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0)
    return Status::FailedPrecondition(
        "compiler worker failed or exhausted wall/RSS/output budget (RSS is "
        "sampled)");
  auto parsed = ParseStrictJson(output);
  if (!parsed.ok())
    return parsed.status();
  auto ms = [](timeval v) {
    return static_cast<uint64_t>(v.tv_sec) * 1000 +
           (static_cast<uint64_t>(v.tv_usec) + 999) / 1000;
  };
  return WorkerResult{std::move(*parsed), peak,
                      ms(usage.ru_utime) + ms(usage.ru_stime), output.size()};
}
} // namespace

Json CoverageJson(const Coverage &c) {
  return Json::Object{{"supported", c.supported},
                      {"scope_closed", c.scope_closed},
                      {"traversal", c.traversal},
                      {"frontier_refs", Strings(c.frontier_refs)},
                      {"reasons", Strings(c.reasons)},
                      {"examined", c.examined}};
}
Json UsageJson(const Usage &u) {
  return Json::Object{{"wall_ms", u.wall_ms},
                      {"cpu_ms", u.cpu_ms},
                      {"peak_rss_bytes", u.peak_rss_bytes},
                      {"output_bytes", u.output_bytes},
                      {"examined", u.examined}};
}
Json RefJson(const EntityRef &r) {
  return Json::Object{{"index_id", r.index_id.value()}, {"ordinal", r.ordinal}};
}
StatusOr<StructuralIndex> BuildStructuralIndex(const ReviewSnapshot &snapshot,
                                               const StructuralBudget &budget) {
  auto start = Clock::now();
  rusage parent_start{};
  getrusage(RUSAGE_SELF, &parent_start);
  if (!budget.max_tus || !budget.max_entities || !budget.max_source_bytes ||
      !budget.wall_ms || !budget.output_bytes || !budget.child_rss_bytes ||
      budget.max_entities > UINT32_MAX || budget.wall_ms > INT64_MAX)
    return Status::InvalidArgument(
        "positive bounded structural budgets required");
  // The VFS has no real-filesystem fallback, including for angled includes,
  // pragmas, modules, PCH and environment-derived include paths.
  std::map<std::string, std::string> files;
  std::map<std::string, ContextId> contexts;
  uint64_t size = 0;
  for (const auto &path : snapshot.captured_paths()) {
    auto bytes = snapshot.Read(path);
    if (!bytes.ok())
      return bytes.status();
    if (bytes->size() > budget.max_source_bytes - size)
      return Status::FailedPrecondition(
          "captured compiler input byte budget exceeded");
    size += bytes->size();
    auto context = snapshot.Context(path, "SOURCE", 0, bytes->size());
    if (context.ok())
      contexts.emplace(path, context->context_id);
    files.emplace(path, std::move(*bytes));
  }
  std::map<std::string, Command> ordered;
  for (const auto &raw : snapshot.compiler_commands()) {
    auto admitted = Admit(snapshot, raw);
    if (!admitted.ok())
      return admitted.status();
    auto key = CanonicalJson(Json::Object{{"file", admitted->file},
                                          {"args", Strings(admitted->args)}});
    if (!key.ok())
      return key.status();
    ordered.emplace(*key, std::move(*admitted));
  }
  Coverage coverage;
  coverage.supported = true;
  coverage.traversal = "COMPLETE";
  coverage.scope_closed =
      snapshot.program_scope().coverage == "DECLARED_CLOSED";
  coverage.frontier_refs = snapshot.program_scope().frontier;
  if (snapshot.compiler_commands().empty())
    Gap(coverage, "compile-commands",
        "no captured compiler command for selected program");
  std::vector<Command> commands;
  for (auto &[key, command] : ordered) {
    if (commands.size() >= budget.max_tus) {
      Gap(coverage, "tu-budget:" + command.file,
          "translation-unit budget exhausted");
      continue;
    }
    commands.push_back(std::move(command));
  }
  auto worker = ParseWorker(commands, files, contexts, budget, start);
  if (!worker.ok())
    return worker.status();
  const auto &object = *worker->document.getAsObject();
  for (const auto &item : *object.getArray("frontier"))
    Gap(coverage, *item.getAsString(),
        "compiler parse or structural traversal incomplete");
  Json::Array command_json;
  for (const auto &command : commands)
    command_json.push_back(
        Json::Object{{"file", command.file}, {"args", Strings(command.args)}});
  auto config =
      CanonicalJson(Json::Object{{"compiler", LLVM_VERSION_STRING},
                                 {"commands", std::move(command_json)},
                                 {"vfs", "captured-only.v1"},
                                 {"max_entities", budget.max_entities}});
  if (!config.ok())
    return config.status();
  ProviderVersion producer{"veritas.clang.local", "local-evidence.v1",
                           HashBytes(*config)};
  coverage.examined = object.getArray("entities")->size();
  auto identity = MakeArtifactId<StructuralIndexTag>(
      Json::Object{{"binding", ToJson(snapshot.binding())},
                   {"config_digest", producer.config_digest},
                   {"structure", worker->document},
                   {"coverage", CoverageJson(coverage)}});
  if (!identity.ok())
    return identity.status();
  auto data = std::make_shared<StructuralIndexData>(
      StructuralIndexData{snapshot,
                          *identity,
                          snapshot.binding(),
                          producer,
                          {},
                          {},
                          coverage,
                          {},
                          {}});
  for (const auto &item : *object.getArray("entities")) {
    const auto &e = *item.getAsObject();
    auto ref = ParseArtifactId<ContextTag>(*e.getString("source_ref"));
    if (!ref.ok())
      return ref.status();
    StructuralEntity entity{
        static_cast<uint32_t>(*e.get("ordinal")->getAsUINT64()),
        *e.getString("kind"),
        *e.getString("name"),
        *e.getString("path"),
        *e.getString("type"),
        *e.get("start")->getAsUINT64(),
        *e.get("end")->getAsUINT64(),
        *ref,
        {},
        *e.getBoolean("direct_call"),
        {}};
    if (auto operand = e.get("operand_index")->getAsUINT64())
      entity.operand_index = static_cast<uint32_t>(*operand);
    for (const auto &operand : *e.getArray("operands"))
      entity.operands.push_back(static_cast<uint32_t>(*operand.getAsUINT64()));
    data->entities.push_back(std::move(entity));
  }
  for (const auto &item : *object.getArray("copies")) {
    const auto &copy = *item.getAsObject();
    CopyRecord record{
        static_cast<uint32_t>(*copy.get("callsite")->getAsUINT64()),
        *copy.getBoolean("admitted"),
        copy.get("extent")->getAsUINT64(),
        {},
        {}};
    for (const auto &guard : *copy.getArray("guards"))
      record.guards.push_back(
          {static_cast<uint32_t>(
               *guard.getAsObject()->get("condition")->getAsUINT64()),
           *guard.getAsObject()->getBoolean("dominates")});
    for (const auto &reason : *copy.getArray("reasons"))
      record.reasons.push_back(*reason.getAsString());
    data->copies.emplace(record.callsite, std::move(record));
  }
  data->metrics.compiler_invocations =
      *object.get("invocations")->getAsUINT64();
  rusage parent_end{};
  getrusage(RUSAGE_SELF, &parent_end);
  auto micros = [](const rusage &u) {
    return static_cast<uint64_t>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) *
               1000000 +
           u.ru_utime.tv_usec + u.ru_stime.tv_usec;
  };
  const auto setup_cpu_ms =
      (micros(parent_end) - micros(parent_start) + 999) / 1000;
  data->usage = {static_cast<uint64_t>(
                     (std::chrono::duration_cast<std::chrono::microseconds>(
                          Clock::now() - start)
                          .count() +
                      999) /
                     1000),
                 worker->cpu_ms + setup_cpu_ms, worker->rss, worker->bytes,
                 coverage.examined};
  return StructuralIndex(std::move(data));
}
const StructuralIndexId &StructuralIndex::id() const { return data_->id; }
const ReviewBinding &StructuralIndex::binding() const { return data_->binding; }
const std::vector<StructuralEntity> &StructuralIndex::entities() const {
  return data_->entities;
}
const Coverage &StructuralIndex::coverage() const { return data_->coverage; }
const Usage &StructuralIndex::usage() const { return data_->usage; }
const StructuralMetrics &StructuralIndex::metrics() const {
  return data_->metrics;
}
StatusOr<ContextArtifact> StructuralIndex::Context(std::string_view path,
                                                   uint64_t start,
                                                   uint64_t end) const {
  auto context = data_->snapshot.Context(path, "SOURCE", start, end);
  if (!context.ok())
    return context.status();
  for (const auto &e : entities())
    if (e.path == context->path && e.start >= start && e.end <= end)
      context->entity_bindings.push_back(
          {{id(), e.ordinal}, e.start, e.end, e.kind});
  Json::Array bindings;
  for (const auto &entity : context->entity_bindings)
    bindings.push_back(Json::Object{{"entity_ref", RefJson(entity.entity_ref)},
                                    {"start", entity.start},
                                    {"end", entity.end},
                                    {"kind", entity.kind}});
  // The plain source context remains the provenance premise. This indexed
  // view additionally commits to the exact index and advertised bindings.
  auto id = MakeArtifactId<ContextTag>(
      Json::Object{{"source_context_id", context->context_id.value()},
                   {"index_id", data_->id.value()},
                   {"entity_bindings", std::move(bindings)}});
  if (!id.ok())
    return id.status();
  context->context_id = *id;
  return context;
}
} // namespace veritas::review
