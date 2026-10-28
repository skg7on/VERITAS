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

#include "veritas/review/SnapshotCapture.h"
#include "clang/Tooling/JSONCompilationDatabase.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <set>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
namespace veritas::review {
namespace fs = std::filesystem;
namespace {
class Fd {
public:
  explicit Fd(int fd = -1) : fd_(fd) {}
  ~Fd() {
    if (fd_ >= 0)
      close(fd_);
  }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  Fd(Fd &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Fd &operator=(Fd &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0)
        close(fd_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }
  int get() const { return fd_; }

private:
  int fd_;
};
template <class Map, class Key>
const typename Map::mapped_type &Lookup(const Map &map, const Key &key) {
  auto it = map.find(key);
  assert(it != map.end());
  return it->second;
}
Status Changed() {
  return Status::FailedPrecondition(
      "retryable: selected inputs changed during snapshot capture");
}
Status Unsafe() {
  return Status::InvalidArgument("unsafe or unsupported symlink/input path");
}
Status Io() {
  return Status::FailedPrecondition("unreadable snapshot input or output");
}
struct Stamp {
  uint64_t device = 0, inode = 0, size = 0;
  int64_t mtime = 0, ctime = 0;
  uint32_t mode = 0;
  bool operator==(const Stamp &) const = default;
};
Stamp StampOf(const struct stat &s) {
#ifdef __APPLE__
  const auto mt = s.st_mtimespec, ct = s.st_ctimespec;
#else
  const auto mt = s.st_mtim, ct = s.st_ctim;
#endif
  return {uint64_t(s.st_dev),
          uint64_t(s.st_ino),
          uint64_t(s.st_size),
          mt.tv_sec * 1000000000LL + mt.tv_nsec,
          ct.tv_sec * 1000000000LL + ct.tv_nsec,
          uint32_t(s.st_mode)};
}
struct FileState {
  std::optional<std::string> bytes;
  Stamp stamp;
  std::string issue;
};
// Every component is opened relative to a pinned directory descriptor. Checking
// canonical strings alone would permit a symlink substitution between checks.
StatusOr<Fd> OpenRelative(int root, const std::string &path, int final_flags) {
  Fd dir(dup(root));
  if (dir.get() < 0)
    return Io();
  fs::path parts(path);
  size_t count = std::distance(parts.begin(), parts.end()), i = 0;
  for (const auto &part : parts) {
    int flags = ++i == count ? final_flags : O_RDONLY | O_DIRECTORY;
    Fd next(openat(dir.get(), part.c_str(),
                   flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (next.get() < 0) {
      if (errno == ELOOP || errno == ENOTDIR)
        return Unsafe();
      if (errno == ENOENT)
        return Status::NotFound("missing captured input");
      return Io();
    }
    dir = std::move(next);
  }
  return dir;
}
StatusOr<FileState> ReadFile(int root, const std::string &path,
                             uint64_t limit) {
  auto fd = OpenRelative(root, path, O_RDONLY);
  if (!fd.ok()) {
    if (fd.status().code() == StatusCode::kInvalidArgument)
      return fd.status();
    return FileState{
        std::nullopt,
        {},
        fd.status().code() == StatusCode::kNotFound ? "missing" : "unreadable"};
  }
  struct stat before{}, after{};
  if (fstat(fd->get(), &before) != 0)
    return Io();
  if (!S_ISREG(before.st_mode))
    return Unsafe();
  if (uint64_t(before.st_size) > limit)
    return Status::FailedPrecondition("snapshot byte budget exceeded");
  std::string bytes;
  std::array<char, 65536> buffer;
  while (true) {
    ssize_t n = read(fd->get(), buffer.data(), buffer.size());
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return Io();
    }
    if (n == 0)
      break;
    if (bytes.size() + uint64_t(n) > limit)
      return Status::FailedPrecondition("snapshot byte budget exceeded");
    bytes.append(buffer.data(), size_t(n));
  }
  if (fstat(fd->get(), &after) != 0)
    return Io();
  if (StampOf(before) != StampOf(after) ||
      bytes.size() != uint64_t(after.st_size))
    return Changed();
  return FileState{std::move(bytes), StampOf(after), {}};
}
Status Walk(int root, const std::string &path, std::set<std::string> &files,
            uint64_t max_files, unsigned depth = 0) {
  if (depth > 128)
    return Unsafe();
  auto fd = OpenRelative(root, path, O_RDONLY);
  if (!fd.ok()) {
    if (fd.status().code() == StatusCode::kNotFound) {
      files.insert(path);
      return Status::Ok();
    }
    return fd.status();
  }
  struct stat stat{};
  if (fstat(fd->get(), &stat) != 0)
    return Io();
  if (S_ISREG(stat.st_mode)) {
    files.insert(path);
    return files.size() > max_files
               ? Status::FailedPrecondition("snapshot file budget exceeded")
               : Status::Ok();
  }
  if (!S_ISDIR(stat.st_mode))
    return Unsafe();
  DIR *dir = fdopendir(dup(fd->get()));
  if (!dir)
    return Io();
  std::vector<std::string> names;
  errno = 0;
  while (auto entry = readdir(dir)) {
    std::string name = entry->d_name;
    if (name != "." && name != "..")
      names.push_back(name);
    if (names.size() > max_files) {
      closedir(dir);
      return Status::FailedPrecondition("snapshot file budget exceeded");
    }
  }
  int saved = errno;
  closedir(dir);
  if (saved)
    return Io();
  std::sort(names.begin(), names.end());
  for (const auto &name : names) {
    auto normalized = NormalizeReviewPath(path + "/" + name);
    if (!normalized.ok())
      return normalized.status();
    auto status = Walk(root, *normalized, files, max_files, depth + 1);
    if (!status.ok())
      return status;
  }
  return Status::Ok();
}
StatusOr<std::string> Git(const fs::path &root,
                          std::vector<std::string> arguments,
                          uint64_t max_bytes = 64 * 1024 * 1024) {
  std::vector<std::string> args = {
      "/usr/bin/git", "--no-optional-locks",      "-c", "core.fsmonitor=false",
      "-c",           "core.hooksPath=/dev/null", "-C", root.string()};
  args.insert(args.end(), arguments.begin(), arguments.end());
  std::vector<char *> argv;
  for (auto &a : args)
    argv.push_back(a.data());
  argv.push_back(nullptr);
  int pipefd[2];
  if (pipe(pipefd) != 0)
    return Io();
  Fd reader(pipefd[0]), writer(pipefd[1]);
  fcntl(reader.get(), F_SETFD, FD_CLOEXEC);
  fcntl(writer.get(), F_SETFD, FD_CLOEXEC);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                   O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, writer.get(), STDOUT_FILENO);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                   O_WRONLY, 0);
  char env0[] = "PATH=/usr/bin:/bin", env1[] = "LC_ALL=C",
       env2[] = "GIT_CONFIG_NOSYSTEM=1", env3[] = "GIT_CONFIG_GLOBAL=/dev/null",
       env4[] = "GIT_TERMINAL_PROMPT=0";
  char *env[] = {env0, env1, env2, env3, env4, nullptr};
  pid_t pid;
  int rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), env);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0)
    return Io();
  writer = Fd();
  fcntl(reader.get(), F_SETFL, O_NONBLOCK);
  std::string out;
  std::array<char, 65536> buf;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  int status = 0;
  bool done = false, eof = false;
  while (!done || !eof) {
    ssize_t n = read(reader.get(), buf.data(), buf.size());
    if (n > 0)
      out.append(buf.data(), size_t(n));
    else if (n == 0)
      eof = true;
    else if (errno != EINTR && errno != EAGAIN) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return Io();
    }
    if (out.size() > max_bytes || std::chrono::steady_clock::now() > deadline) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return Status::FailedPrecondition(
          "bounded Git capture exceeded its output/deadline");
    }
    if (!done) {
      pid_t result = waitpid(pid, &status, WNOHANG);
      if (result < 0)
        return Io();
      done = result == pid;
    }
    if (!done || !eof) {
      pollfd p{reader.get(), POLLIN, 0};
      poll(&p, 1, 5);
    }
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return Status::FailedPrecondition("Git snapshot input unavailable");
  return out;
}
std::vector<std::string> SplitNul(const std::string &value) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start < value.size()) {
    auto end = value.find('\0', start);
    if (end == std::string::npos)
      break;
    out.push_back(value.substr(start, end - start));
    start = end + 1;
  }
  return out;
}
StatusOr<std::string> PinBase(const SnapshotRequest &r, const fs::path &root) {
  if (r.base_ref.empty() || r.base_ref.front() == '-' ||
      r.base_ref.find('\0') != std::string::npos ||
      r.base_ref.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ"
                                   "RSTUVWXYZ0123456789_./~^{}@+-") !=
          std::string::npos)
    return Status::InvalidArgument("invalid Git base ref");
  auto value = Git(
      root,
      {"rev-parse", "--verify", "--end-of-options", r.base_ref + "^{commit}"},
      256);
  if (!value.ok())
    return value.status();
  while (value->ends_with('\n'))
    value->pop_back();
  if (value->size() != 40 && value->size() != 64)
    return Io();
  return value;
}
StatusOr<std::string> RelativeInput(const fs::path &root, const fs::path &cwd,
                                    const std::string &text) {
  fs::path path(text);
  if (path.is_relative())
    path = cwd / path;
  auto rel = path.lexically_normal().lexically_relative(root);
  if (rel.empty())
    return Unsafe();
  return NormalizeReviewPath(rel.generic_string());
}

// Only compiler-defined path operands relocate. Semantic data (notably macro
// replacement tokens) remains literal. Typed objects cannot alias a literal
// marker such as "@REPO@" or a relative directory with that name.
Json PathIdentity(const fs::path &root, const fs::path &cwd,
                  const std::string &text) {
  fs::path path(text);
  if (path.is_relative())
    path = cwd / path;
  path = path.lexically_normal();
  auto relative = path.lexically_relative(root);
  bool local = !relative.empty();
  for (const auto &part : relative)
    if (part == "..")
      local = false;
  if (local)
    return Json::Object{{"root", "REPOSITORY"},
                        {"path", relative.generic_string()}};
  return Json::Object{{"root", "EXTERNAL"}, {"path", path.generic_string()}};
}
Json::Array CommandIdentity(const std::vector<std::string> &command,
                            const fs::path &root, const fs::path &cwd,
                            const std::string &source) {
  static const std::set<std::string> path_flags = {
      "-I",       "-isystem",  "-iquote",   "-idirafter",    "-include",
      "-imacros", "-isysroot", "--sysroot", "-resource-dir", "-o",
      "-MF"};
  static const std::set<std::string> data_flags = {
      "-D",     "-U",  "-x", "-target", "--target", "-Xclang", "-Xpreprocessor",
      "-mllvm", "-MT", "-MQ"};
  static const std::set<std::string> no_operand = {
      "-c",  "-S",  "-E",  "-pipe", "-fPIC", "-fPIE", "-shared", "-pthread",
      "-g",  "-g0", "-g1", "-g2",   "-g3",   "-ggdb", "-O0",     "-O1",
      "-O2", "-O3", "-Os", "-Oz",   "-Og",   "-Ofast"};
  Json::Array result;
  bool path_next = false, literal_next = false;
  for (size_t i = 0; i < command.size(); ++i) {
    const auto &arg = command[i];
    if (path_next) {
      result.push_back(
          Json::Object{{"path_operand", PathIdentity(root, cwd, arg)}});
      path_next = false;
      continue;
    }
    if (literal_next) {
      result.push_back(arg);
      literal_next = false;
      continue;
    }
    if (path_flags.contains(arg)) {
      result.push_back(arg);
      path_next = true;
      continue;
    }
    if (data_flags.contains(arg)) {
      result.push_back(arg);
      literal_next = true;
      continue;
    }
    bool joined = false;
    for (auto prefix : {"-I", "--sysroot=", "-resource-dir="}) {
      std::string_view flag = prefix;
      if (arg.starts_with(flag) && arg.size() > flag.size()) {
        result.push_back(Json::Object{
            {"path_option", std::string(flag)},
            {"operand", PathIdentity(root, cwd, arg.substr(flag.size()))}});
        joined = true;
        break;
      }
    }
    if (joined)
      continue;
    if (arg.starts_with('@')) {
      result.push_back(Json::Object{
          {"response_file", PathIdentity(root, cwd, arg.substr(1))}});
      continue;
    }
    if (i == 0 && fs::path(arg).is_absolute()) {
      result.push_back(
          Json::Object{{"compiler_path", PathIdentity(root, cwd, arg)}});
      continue;
    }
    if (!arg.starts_with('-')) {
      auto relative = RelativeInput(root, cwd, arg);
      if (relative.ok() && *relative == source) {
        result.push_back(
            Json::Object{{"source_path", PathIdentity(root, cwd, arg)}});
        continue;
      }
    }
    result.push_back(arg);
    // Unknown options have unknown arity: preserve their next operand exactly.
    // This is identity encoding, not permission to execute an option.
    if (i > 0 && arg.starts_with('-') && !no_operand.contains(arg) &&
        !arg.starts_with("-W") && arg.find('=') == std::string::npos)
      literal_next = true;
  }
  return result;
}
Json::Array Strings(const std::vector<std::string> &values) {
  Json::Array result;
  for (const auto &v : values)
    result.push_back(v);
  return result;
}
} // namespace
struct SnapshotData {
  SnapshotRequest request;
  fs::path root;
  std::string base_oid, index_bytes, untracked_bytes;
  ReviewBinding binding;
  ProgramScope scope;
  Coverage compiler;
  std::vector<CapturedCompileCommand> compiler_commands;
  std::map<std::string, std::shared_ptr<const std::string>> blobs;
  std::map<std::string, std::string> files, base_files;
  std::map<std::string, FileState> captured;
  std::vector<std::string> excluded, unavailable;
  std::set<std::string> artifact_members;
};
StatusOr<ReviewSnapshot> CaptureReviewSnapshot(const SnapshotRequest &request) {
  if (request.max_files == 0 || request.max_bytes == 0)
    return Status::InvalidArgument("positive snapshot limits required");
  auto d = std::make_shared<SnapshotData>();
  d->request = request;
  std::error_code ec;
  d->root = fs::canonical(request.project_root, ec);
  if (ec)
    return Io();
  Fd root(
      open(d->root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (root.get() < 0)
    return Io();
  auto base = PinBase(request, d->root);
  if (!base.ok())
    return base.status();
  d->base_oid = *base;
  auto index = Git(d->root, {"ls-files", "-z", "--cached"});
  if (!index.ok())
    return index.status();
  d->index_bytes = *index;
  auto untracked =
      Git(d->root, {"ls-files", "-z", "--others", "--exclude-standard"});
  if (!untracked.ok())
    return untracked.status();
  d->untracked_bytes = *untracked;
  auto tree = Git(d->root, {"ls-tree", "-r", "-z", *base});
  if (!tree.ok())
    return tree.status();
  std::map<std::string, std::string> base_objects;
  std::set<std::string> unsupported_base;
  std::set<std::string> tracked;
  for (const auto &path : SplitNul(*index)) {
    auto p = NormalizeReviewPath(path);
    if (!p.ok())
      return p.status();
    tracked.insert(*p);
  }
  for (const auto &entry : SplitNul(*tree)) {
    auto tab = entry.find('\t'), space = entry.rfind(' ', tab);
    if (tab == std::string::npos || space == std::string::npos)
      return Io();
    auto p = NormalizeReviewPath(entry.substr(tab + 1));
    if (!p.ok())
      return p.status();
    base_objects.emplace(*p, entry.substr(space + 1, tab - space - 1));
    if (!entry.starts_with("100644 blob ") &&
        !entry.starts_with("100755 blob "))
      unsupported_base.insert(*p);
  }
  std::set<std::string> selected;
  if (request.selected_tracked.empty()) {
    selected = tracked;
    for (const auto &[path, object] : base_objects)
      selected.insert(path);
  } else
    for (const auto &path : request.selected_tracked) {
      auto p = NormalizeReviewPath(path);
      if (!p.ok())
        return p.status();
      if (!tracked.contains(*p) && !base_objects.contains(*p))
        return Status::InvalidArgument("selected tracked input is not tracked");
      selected.insert(*p);
    }
  for (const auto &path : selected)
    if (unsupported_base.contains(path))
      return Unsafe();
  std::set<std::string> admitted_untracked;
  for (const auto &path : request.selected_untracked) {
    auto p = NormalizeReviewPath(path);
    if (!p.ok())
      return p.status();
    if (tracked.contains(*p))
      return Status::InvalidArgument(
          "tracked path supplied as untracked selection");
    selected.insert(*p);
    admitted_untracked.insert(*p);
  }
  std::set<std::string> artifact_paths;
  for (const auto &path : request.selected_artifacts) {
    auto p = NormalizeReviewPath(path);
    if (!p.ok())
      return p.status();
    auto st = Walk(root.get(), *p, artifact_paths, request.max_files);
    if (!st.ok())
      return st;
  }
  d->artifact_members = artifact_paths;
  auto select_special = [&](const std::string &raw) -> Status {
    if (raw.empty())
      return Status::Ok();
    auto p = NormalizeReviewPath(raw);
    if (!p.ok())
      return p.status();
    artifact_paths.insert(*p);
    selected.insert(*p);
    return Status::Ok();
  };
  for (const auto &raw :
       {request.compile_commands_path, request.program_manifest_path}) {
    auto s = select_special(raw);
    if (!s.ok())
      return s;
  }
  selected.insert(artifact_paths.begin(), artifact_paths.end());
  uint64_t total = 0;
  auto capture = [&](const std::string &path) -> Status {
    if (d->captured.contains(path))
      return Status::Ok();
    if (d->captured.size() >= request.max_files)
      return Status::FailedPrecondition("snapshot file budget exceeded");
    auto state = ReadFile(root.get(), path, request.max_bytes - total);
    if (!state.ok())
      return state.status();
    if (state->bytes) {
      total += state->bytes->size();
      auto digest = HashBytes(*state->bytes);
      d->files[path] = digest;
      d->blobs.try_emplace(digest,
                           std::make_shared<const std::string>(*state->bytes));
    } else
      d->unavailable.push_back(state->issue + ":" + path);
    d->captured.emplace(path, std::move(*state));
    return Status::Ok();
  };
  for (const auto &path : selected) {
    auto st = capture(path);
    if (!st.ok())
      return st;
  }
  ProgramScopeInput scope_input;
  scope_input.target_key = request.program_target_key;
  scope_input.membership_source = request.membership_source;
  if (!request.program_manifest_path.empty()) {
    auto path = *NormalizeReviewPath(request.program_manifest_path);
    auto it = d->files.find(path);
    if (it == d->files.end())
      return Status::FailedPrecondition(
          "selected program manifest unavailable");
    auto parsed = ParseStrictJson(*Lookup(d->blobs, it->second));
    if (!parsed.ok())
      return parsed.status();
    auto paths = DeclaredProgramPaths(*parsed, request.program_target_key);
    if (!paths.ok())
      return paths.status();
    scope_input.manifest = std::move(*parsed);
    for (const auto &member : *paths) {
      // A declaration can select build artifacts, but never silently admits an
      // untracked source. Explicit artifact/untracked selection is required.
      if (!tracked.contains(member) && !base_objects.contains(member) &&
          !admitted_untracked.contains(member) &&
          !artifact_paths.contains(member)) {
        d->unavailable.push_back("unselected:" + member);
        continue;
      }
      selected.insert(member);
      auto st = capture(member);
      if (!st.ok())
        return st;
    }
  }
  std::map<std::string, std::vector<std::string>> commands;
  Json::Array command_identity;
  if (!request.compile_commands_path.empty()) {
    auto path = *NormalizeReviewPath(request.compile_commands_path);
    auto it = d->files.find(path);
    if (it != d->files.end()) {
      const auto &raw = *Lookup(d->blobs, it->second);
      auto strict = ParseStrictJson(raw);
      if (!strict.ok())
        return strict.status();
      std::string error;
      auto db = clang::tooling::JSONCompilationDatabase::loadFromBuffer(
          raw, error, clang::tooling::JSONCommandLineSyntax::AutoDetect);
      if (!db)
        return Status::InvalidArgument(
            "invalid captured compilation database: " + error);
      for (const auto &command : db->getAllCompileCommands()) {
        fs::path cwd(command.Directory);
        if (cwd.is_relative())
          cwd = d->root / cwd;
        cwd = cwd.lexically_normal();
        auto file = RelativeInput(d->root, cwd, command.Filename);
        if (!file.ok())
          return file.status();
        auto args = CommandIdentity(command.CommandLine, d->root, cwd, *file);
        auto encoded = CanonicalJson(Json::Object{
            {"file", *file},
            {"directory", PathIdentity(d->root, d->root, cwd.generic_string())},
            {"arguments", std::move(args)}});
        if (!encoded.ok())
          return encoded.status();
        d->compiler_commands.push_back(
            {cwd.generic_string(), *file, command.CommandLine});
        commands[*file].push_back(HashBytes(*encoded));
        command_identity.push_back(*encoded);
        if (!d->files.contains(*file))
          d->compiler.frontier_refs.push_back("uncaptured-source:" + *file);
        // Discover explicit include/configuration requirements without
        // executing a compiler, plugin, response file or any command from the
        // project.
        for (size_t i = 1; i < command.CommandLine.size(); ++i) {
          std::string flag = command.CommandLine[i], value;
          bool directory = false;
          if (flag == "-I" || flag == "-isystem" || flag == "-iquote" ||
              flag == "-idirafter" || flag == "-isysroot" ||
              flag == "--sysroot" || flag == "-resource-dir") {
            directory = true;
            if (i + 1 < command.CommandLine.size())
              value = command.CommandLine[++i];
          } else if (flag == "-include" || flag == "-imacros") {
            if (i + 1 < command.CommandLine.size())
              value = command.CommandLine[++i];
          } else if (flag.starts_with("-I") && flag.size() > 2) {
            directory = true;
            value = flag.substr(2);
          } else if (flag.starts_with('@'))
            value = flag.substr(1);
          else
            continue;
          if (value.empty()) {
            d->compiler.frontier_refs.push_back("missing-flag-argument:" +
                                                flag);
            continue;
          }
          auto input = RelativeInput(d->root, cwd, value);
          if (!input.ok()) {
            d->compiler.frontier_refs.push_back("external-input:" + value);
            continue;
          }
          bool covered =
              directory
                  ? std::any_of(d->files.begin(), d->files.end(),
                                [&](const auto &x) {
                                  return x.first.starts_with(*input + "/");
                                })
                  : d->files.contains(*input);
          if (!covered)
            d->compiler.frontier_refs.push_back("uncaptured-input:" + *input);
        }
      }
    }
  }
  // Every selected native source gets a path-sensitive TU digest; commands are
  // a set so build-database order cannot alter membership identity.
  for (const auto &[path, digest] : d->files) {
    if (!scope_input.manifest && !commands.contains(path))
      continue;
    auto hashes = commands[path];
    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
    auto bytes = CanonicalJson(Json::Object{
        {"path", path}, {"content", digest}, {"commands", Strings(hashes)}});
    if (!bytes.ok())
      return bytes.status();
    scope_input.translation_unit_digests[path] = HashBytes(*bytes);
  }
  scope_input.artifact_digests = d->files;
  auto scope = ResolveProgramScope(scope_input);
  if (!scope.ok())
    return scope.status();
  d->scope = std::move(*scope);
  std::erase_if(d->compiler_commands, [&](const auto &command) {
    auto it = scope_input.translation_unit_digests.find(command.file);
    return it == scope_input.translation_unit_digests.end() ||
           std::find(d->scope.member_tu_digests.begin(),
                     d->scope.member_tu_digests.end(), it->second) ==
               d->scope.member_tu_digests.end();
  });
  auto scope_id = MakeArtifactId<ProgramScopeTag>(ToJson(d->scope));
  if (!scope_id.ok())
    return scope_id.status();
  for (const auto &path : SplitNul(*untracked))
    if (!selected.contains(path))
      d->excluded.push_back(path);
  for (const auto &path : tracked)
    if (!selected.contains(path))
      d->excluded.push_back(path);
  std::sort(d->excluded.begin(), d->excluded.end());
  std::sort(d->unavailable.begin(), d->unavailable.end());
  for (const auto &[path, object] : base_objects)
    if (selected.contains(path)) {
      if (unsupported_base.contains(path))
        return Unsafe();
      auto bytes =
          Git(d->root, {"cat-file", "blob", object}, request.max_bytes - total);
      if (!bytes.ok())
        return bytes.status();
      total += bytes->size();
      auto digest = HashBytes(*bytes);
      d->base_files[path] = digest;
      d->blobs.try_emplace(
          digest, std::make_shared<const std::string>(std::move(*bytes)));
    }
  // Coherent double collection: stamps include inode and ctime, so restoring
  // size/mtime/content does not hide an intervening write or replacement.
  for (const auto &[path, state] : d->captured) {
    auto now = ReadFile(root.get(), path, request.max_bytes);
    if (!now.ok()) {
      if (now.status().code() == StatusCode::kInvalidArgument)
        return now.status();
      return Changed();
    }
    if (now->bytes != state.bytes || now->stamp != state.stamp ||
        now->issue != state.issue)
      return Changed();
  }
  std::set<std::string> again;
  for (const auto &raw : request.selected_artifacts) {
    auto path = *NormalizeReviewPath(raw);
    auto status = Walk(root.get(), path, again, request.max_files);
    if (!status.ok())
      return Changed();
  }
  if (again != d->artifact_members)
    return Changed();
  auto index_after = Git(d->root, {"ls-files", "-z", "--cached"});
  auto base_after = PinBase(request, d->root);
  auto untracked_after =
      Git(d->root, {"ls-files", "-z", "--others", "--exclude-standard"});
  struct stat pinned_root{}, live_root{};
  if (fstat(root.get(), &pinned_root) != 0 ||
      lstat(d->root.c_str(), &live_root) != 0 ||
      pinned_root.st_dev != live_root.st_dev ||
      pinned_root.st_ino != live_root.st_ino)
    return Changed();
  if (!index_after.ok() || !base_after.ok() || *index_after != *index ||
      *base_after != *base)
    return Changed();
  std::sort(command_identity.begin(), command_identity.end(),
            [](const Json &a, const Json &b) {
              return *a.getAsString() < *b.getAsString();
            });
  auto build_bytes = CanonicalJson(command_identity);
  if (!build_bytes.ok())
    return build_bytes.status();
  const std::string compile_semantic_digest = HashBytes(*build_bytes);
  Json::Object files, base_files;
  for (const auto &[p, digest] : d->files)
    files[p] = (!request.compile_commands_path.empty() &&
                p == *NormalizeReviewPath(request.compile_commands_path))
                   ? compile_semantic_digest
                   : digest;
  for (const auto &[p, digest] : d->base_files)
    base_files[p] = digest;
  auto working_bytes = CanonicalJson(files);
  if (!working_bytes.ok())
    return working_bytes.status();
  Json::Array providers;
  auto versions = request.provider_versions;
  std::sort(versions.begin(), versions.end());
  for (const auto &v : versions)
    providers.push_back(Json::Object{{"name", v.name},
                                     {"version", v.version},
                                     {"config_digest", v.config_digest}});
  Json::Object build_artifacts;
  for (const auto &path : artifact_paths) {
    auto it = files.find(path);
    if (it != files.end())
      build_artifacts[path] = it->second;
  }
  auto build_context =
      CanonicalJson(Json::Object{{"commands", command_identity},
                                 {"artifacts", std::move(build_artifacts)},
                                 {"providers", std::move(providers)}});
  if (!build_context.ok())
    return build_context.status();
  d->binding.repository_id = request.repository_id;
  d->binding.base_revision_id = "git:" + *base;
  d->binding.working_tree_digest = HashBytes(*working_bytes);
  d->binding.program_scope_id = *scope_id;
  d->binding.build_context_digest = HashBytes(*build_context);
  d->binding.provider_versions = std::move(versions);
  auto identity = MakeArtifactId<SnapshotTag>(
      Json::Object{{"repository_id", request.repository_id},
                   {"base_revision_id", d->binding.base_revision_id},
                   {"base_files", std::move(base_files)},
                   {"files", std::move(files)},
                   {"program_scope_id", scope_id->value()},
                   {"build_context_digest", d->binding.build_context_digest},
                   {"excluded", Strings(d->excluded)},
                   {"unavailable", Strings(d->unavailable)}});
  if (!identity.ok())
    return identity.status();
  d->binding.snapshot_id = *identity;
  auto valid = ValidateWireType(ToJson(d->binding), "ReviewBinding");
  if (!valid.ok())
    return valid;
  d->compiler.supported = false;
  d->compiler.scope_closed = d->scope.coverage == "DECLARED_CLOSED";
  d->compiler.examined = d->captured.size();
  d->compiler.reasons = {"compiler execution and transitive include closure "
                         "require the structural provider"};
  d->compiler.frontier_refs.insert(d->compiler.frontier_refs.end(),
                                   d->unavailable.begin(),
                                   d->unavailable.end());
  return ReviewSnapshot(std::move(d));
}
const std::vector<CapturedCompileCommand> &
ReviewSnapshot::compiler_commands() const {
  return data_->compiler_commands;
}
std::vector<std::string> ReviewSnapshot::captured_paths() const {
  std::vector<std::string> paths;
  for (const auto &[path, digest] : data_->files) paths.push_back(path);
  return paths;
}
StatusOr<std::string>
ReviewSnapshot::CompilerPath(std::string_view directory,
                             std::string_view text) const {
  if (text.empty() || text.find('\0') != std::string_view::npos ||
      text.find('\\') != std::string_view::npos)
    return Unsafe();
  fs::path cwd(directory);
  if (cwd.is_relative())
    cwd = data_->root / cwd;
  fs::path path(text);
  if (path.is_relative())
    path = cwd / path;
  path = path.lexically_normal();
  if (path == data_->root)
    return std::string(".");
  return RelativeInput(data_->root, cwd, std::string(text));
}
const ReviewBinding &ReviewSnapshot::binding() const { return data_->binding; }
const ProgramScope &ReviewSnapshot::program_scope() const {
  return data_->scope;
}
const Coverage &ReviewSnapshot::compiler_coverage() const {
  return data_->compiler;
}
const std::vector<std::string> &ReviewSnapshot::excluded() const {
  return data_->excluded;
}
const std::vector<std::string> &ReviewSnapshot::unavailable() const {
  return data_->unavailable;
}
StatusOr<std::string> ReviewSnapshot::Read(std::string_view path) const {
  auto p = NormalizeReviewPath(path);
  if (!p.ok())
    return p.status();
  auto it = data_->files.find(*p);
  if (it == data_->files.end())
    return Status::NotFound("input not captured");
  return *Lookup(data_->blobs, it->second);
}
StatusOr<std::string> ReviewSnapshot::ReadBase(std::string_view path) const {
  auto p = NormalizeReviewPath(path);
  if (!p.ok())
    return p.status();
  auto it = data_->base_files.find(*p);
  if (it == data_->base_files.end())
    return Status::NotFound("base input not captured");
  return *Lookup(data_->blobs, it->second);
}
StatusOr<ContextArtifact> ReviewSnapshot::Context(std::string_view path,
                                                  std::string_view kind,
                                                  uint64_t start,
                                                  uint64_t end) const {
  if (kind != "SOURCE" && kind != "TEST" && kind != "DOC")
    return Status::InvalidArgument(
        "context kind requires a registered producer");
  auto bytes = Read(path);
  if (!bytes.ok())
    return bytes.status();
  if (start > end || end > bytes->size())
    return Status::InvalidArgument("invalid context byte span");
  auto p = *NormalizeReviewPath(path);
  auto excerpt = bytes->substr(start, end - start);
  std::vector<Region> omitted;
  if (start > 0)
    omitted.push_back({0, start});
  if (end < bytes->size())
    omitted.push_back({end, uint64_t(bytes->size())});
  Json::Array omitted_json;
  for (const auto &region : omitted)
    omitted_json.push_back(
        Json::Object{{"start", region.start}, {"end", region.end}});
  Json::Object semantic{
      {"snapshot_id", data_->binding.snapshot_id->value()},
      {"kind", std::string(kind)},
      {"path", p},
      {"content_digest", Lookup(data_->files, p)},
      {"start", start},
      {"end", end},
      {"excerpt", excerpt},
      {"omitted_regions", std::move(omitted_json)},
      {"selection_reason", "operator-selected captured input"},
      {"entity_bindings", Json::Array{}}};
  auto id = MakeArtifactId<ContextTag>(semantic);
  if (!id.ok())
    return id.status();
  return ContextArtifact{*id,
                         data_->binding,
                         std::string(kind),
                         p,
                         Lookup(data_->files, p),
                         start,
                         end,
                         excerpt,
                         std::move(omitted),
                         "operator-selected captured input",
                         {}};
}
StatusOr<bool> ReviewSnapshot::LiveTreeAdvanced() const {
  Fd root(open(data_->root.c_str(),
               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (root.get() < 0)
    return true;
  for (const auto &[path, state] : data_->captured) {
    auto now = ReadFile(root.get(), path, data_->request.max_bytes);
    if (!now.ok() || now->bytes != state.bytes || now->issue != state.issue)
      return true;
  }
  std::set<std::string> members;
  for (const auto &raw : data_->request.selected_artifacts) {
    auto path = *NormalizeReviewPath(raw);
    auto status = Walk(root.get(), path, members, data_->request.max_files);
    if (!status.ok())
      return true;
  }
  if (members != data_->artifact_members)
    return true;
  auto index = Git(data_->root, {"ls-files", "-z", "--cached"});
  auto base = PinBase(data_->request, data_->root);
  auto untracked =
      Git(data_->root, {"ls-files", "-z", "--others", "--exclude-standard"});
  return !index.ok() || !base.ok() || !untracked.ok() ||
         *index != data_->index_bytes || *base != data_->base_oid ||
         *untracked != data_->untracked_bytes;
}
Status ReviewSnapshot::Materialize(const fs::path &destination) const {
  if (!destination.is_absolute() || destination.filename().empty())
    return Unsafe();
  auto normalized = destination.lexically_normal();
  for (const auto &p : destination)
    if (p == "..")
      return Unsafe();
  Fd slash(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  auto parent = OpenRelative(
      slash.get(), normalized.parent_path().relative_path().generic_string(),
      O_RDONLY | O_DIRECTORY);
  if (!parent.ok())
    return parent.status();
  if (mkdirat(parent->get(), normalized.filename().c_str(), 0700) != 0)
    return Status::FailedPrecondition(
        "worker destination must be new and private");
  Fd root(openat(parent->get(), normalized.filename().c_str(),
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (root.get() < 0)
    return Io();
  std::set<std::string> dirs;
  for (const auto &[path, digest] : data_->files) {
    Fd dir(dup(root.get()));
    fs::path p(path);
    std::string relative;
    for (auto it = p.begin(); it != p.end(); ++it) {
      bool final = std::next(it) == p.end();
      if (final) {
        Fd output(openat(dir.get(), it->c_str(),
                         O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                         0600));
        if (output.get() < 0)
          return Io();
        const auto &bytes = *Lookup(data_->blobs, digest);
        if (HashBytes(bytes) != digest)
          return Status::Internal("corrupt snapshot blob");
        size_t written = 0;
        while (written < bytes.size()) {
          ssize_t n = write(output.get(), bytes.data() + written,
                            bytes.size() - written);
          if (n < 0 && errno == EINTR)
            continue;
          if (n <= 0)
            return Io();
          written += size_t(n);
        }
        if (fchmod(output.get(), 0400) != 0)
          return Io();
      } else {
        relative += (relative.empty() ? "" : "/") + it->string();
        if (mkdirat(dir.get(), it->c_str(), 0700) != 0 && errno != EEXIST)
          return Io();
        Fd child(openat(dir.get(), it->c_str(),
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (child.get() < 0)
          return Unsafe();
        dir = std::move(child);
        dirs.insert(relative);
      }
    }
  }
  for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
    auto fd = OpenRelative(root.get(), *it, O_RDONLY | O_DIRECTORY);
    if (!fd.ok())
      return fd.status();
    if (fchmod(fd->get(), 0500) != 0)
      return Io();
  }
  if (fchmod(root.get(), 0500) != 0)
    return Io();
  return Status::Ok();
}
} // namespace veritas::review
