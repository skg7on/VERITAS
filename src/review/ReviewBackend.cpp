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

#include "veritas/review/ReviewBackend.h"
#include "veritas/review/Verification.h"
#include <algorithm>
#include <chrono>
namespace veritas::review {
namespace {
using O = Json::Object;
using A = Json::Array;
std::string Text(const O &o, std::string_view k) { return *o.getString(k); }
uint64_t Number(const O &o, std::string_view k) {
  return *o.get(k)->getAsUINT64();
}
A Strings(const std::vector<std::string> &items) {
  A a;
  for (const auto &s : items)
    a.emplace_back(s);
  return a;
}
Json Ref(const EntityRef &r) {
  return O{{"index_id", r.index_id.value()}, {"ordinal", r.ordinal}};
}
Json CoverageJson(const Coverage &c) {
  return O{{"supported", c.supported},
           {"scope_closed", c.scope_closed},
           {"traversal", c.traversal},
           {"frontier_refs", Strings(c.frontier_refs)},
           {"reasons", Strings(c.reasons)},
           {"examined", c.examined}};
}
Json UsageJson(const Usage &u) {
  return O{{"wall_ms", u.wall_ms},
           {"cpu_ms", u.cpu_ms},
           {"peak_rss_bytes", u.peak_rss_bytes},
           {"output_bytes", u.output_bytes},
           {"examined", u.examined}};
}
Json ContextJson(const ContextArtifact &c) {
  A omitted, entities;
  for (auto &v : c.omitted_regions)
    omitted.emplace_back(O{{"start", v.start}, {"end", v.end}});
  for (auto &v : c.entity_bindings)
    entities.emplace_back(O{{"entity_ref", Ref(v.entity_ref)},
                            {"start", v.start},
                            {"end", v.end},
                            {"kind", v.kind}});
  return O{{"context_id", c.context_id.value()},
           {"binding", ToJson(c.binding)},
           {"kind", c.kind},
           {"path", c.path},
           {"content_digest", c.content_digest},
           {"start", c.start},
           {"end", c.end},
           {"excerpt", c.excerpt},
           {"omitted_regions", omitted},
           {"selection_reason", c.selection_reason},
           {"entity_bindings", entities}};
}
Json Capabilities(const BackendConfig &c, bool captured) {
  A a;
  for (auto op :
       {"capture", "context", "seed-candidates", "local-evidence", "release"}) {
    if (!captured && std::string_view(op) != "capture")
      continue;
    A predicates;
    if (std::string_view(op) == "local-evidence" ||
        std::string_view(op) == "seed-candidates")
      predicates.emplace_back("copy_bound.v1");
    a.emplace_back(O{{"operation", op},
                     {"version", "2"},
                     {"predicate_kinds", predicates},
                     {"authority", "OBSERVATION"},
                     {"limits", O{{"wall_ms", c.wall_ms},
                                  {"output_bytes", c.max_output_bytes},
                                  {"examined", c.max_examined}}}});
  }
  return a;
}
Json Success(std::string_view id, Json result) {
  return O{{"protocol", kProtocol},
           {"request_id", id},
           {"ok", true},
           {"result", std::move(result)}};
}
Json Failure(std::string_view id, std::string_view code,
             std::string_view message) {
  return O{{"protocol", kProtocol},
           {"request_id", id},
           {"ok", false},
           {"error", O{{"code", code},
                       {"message", message},
                       {"retryable", false},
                       {"details", A{}}}}};
}
bool Keys(const O &o, std::initializer_list<std::string_view> keys) {
  if (o.size() != keys.size())
    return false;
  for (auto k : keys)
    if (!o.get(k))
      return false;
  return true;
}
} // namespace
struct ReviewBackend::State {
  ReviewSnapshot snapshot;
  StructuralIndex index;
  std::map<std::string, ContextArtifact> contexts;
  Coverage coverage;
  std::map<uint32_t, std::string> callsite_contexts;
};
ReviewBackend::ReviewBackend(BackendConfig c) : config_(std::move(c)) {}
ReviewBackend::~ReviewBackend() = default;
size_t ReviewBackend::published_snapshots() const { return state_ ? 1 : 0; }
StatusOr<std::string> ReviewBackend::Call(std::string_view bytes) {
  if (bytes.size() > config_.max_request_bytes)
    return Status::InvalidArgument("request size limit");
  auto decoded = DecodeV2(bytes);
  if (!decoded.ok())
    return decoded.status();
  if (decoded->ok.has_value())
    return Status::InvalidArgument("expected request");
  const auto &id = decoded->request_id;
  const auto &op = decoded->operation;
  const auto &q = *decoded->document.getAsObject()->getObject("parameters");
  auto fail = [&](std::string_view code,
                  std::string_view message) -> StatusOr<std::string> {
    auto response = CanonicalJson(Failure(id, code, message));
    if (response.ok() && response->size() > config_.max_output_bytes)
      return Status::FailedPrecondition("failure output limit");
    return response;
  };
  auto start = std::chrono::steady_clock::now();
  auto elapsed = [&]() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
  };
  struct Charge {
    uint64_t &total;
    std::chrono::steady_clock::time_point start;
    ~Charge() {
      total += static_cast<uint64_t>(
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count()) +
               1;
    }
  } charge{charged_wall_ms_, start};
  if (charged_wall_ms_ >= config_.wall_ms)
    return fail("LIMIT_EXCEEDED", "aggregate backend deadline");
  const uint64_t remaining = config_.wall_ms - charged_wall_ms_;
  auto finish = [&](Json result) -> StatusOr<std::string> {
    if (elapsed() >= remaining)
      return fail("LIMIT_EXCEEDED", "aggregate backend deadline");
    auto response = CanonicalJson(Success(id, std::move(result)));
    if (!response.ok())
      return response.status();
    if (response->size() > config_.max_output_bytes)
      return fail("LIMIT_EXCEEDED", "response output limit");
    auto checked = DecodeV2(*response, id, op);
    if (!checked.ok())
      return checked.status();
    return response;
  };
  if (!config_.max_request_bytes || !config_.max_output_bytes ||
      !config_.max_context_bytes || !config_.max_examined ||
      !config_.max_requests || !config_.wall_ms)
    return fail("INVALID_CONFIG", "positive limits required");
  if (request_ids_.size() >= config_.max_requests)
    return fail("LIMIT_EXCEEDED", "session request limit");
  if (!request_ids_.insert(id).second)
    return fail("DUPLICATE_REQUEST", "request ID already used");
  if (op == "handshake") {
    bool found = false;
    for (const auto &p : *q.getArray("accepted_protocols"))
      if (*p.getAsString() == kProtocol)
        found = true;
    if (!found)
      return fail("UNSUPPORTED", "backend v2 negotiation required");
    auto response =
        finish(O{{"protocols", A{std::string(kProtocol)}},
                 {"backend_version", "rdx07.v1"},
                 {"capabilities", Capabilities(config_, bool(state_))}});
    if (response.ok() && *DecodeV2(*response)->ok)
      negotiated_ = true;
    return response;
  }
  if (!negotiated_)
    return fail("FAILED_PRECONDITION", "handshake required before operations");
  if (op == "capture") {
    if (state_)
      return fail("FAILED_PRECONDITION",
                  "release pinned snapshot before capture");
    auto found = config_.projects.find(Text(q, "project_key"));
    if (found == config_.projects.end())
      return fail("INVALID_SELECTION", "unknown trusted project key");
    const auto &project = found->second;
    SnapshotRequest request = project.snapshot;
    if (Text(q, "base_ref") != request.base_ref)
      return fail("INVALID_SELECTION", "base differs from trusted selection");
    request.selected_untracked.clear();
    for (const auto &item : *q.getArray("selected_untracked")) {
      auto path = *item.getAsString();
      if (!project.allowed_untracked.contains(path))
        return fail("INVALID_SELECTION",
                    "untracked path not selected by operator");
      request.selected_untracked.push_back(path);
    }
    if (q.get("program_manifest_key")->kind() != Json::Null) {
      auto program = project.programs.find(Text(q, "program_manifest_key"));
      if (program == project.programs.end())
        return fail("INVALID_SELECTION", "unknown program key");
      request.program_manifest_path = program->second.first;
      request.program_target_key = program->second.second;
    }
    if (q.get("target_ref")->kind() != Json::Null)
      return fail("UNSUPPORTED",
                  "only captured working-tree targets are supported");
    std::vector<ContextSelection> selections;
    for (const auto &key : *q.getArray("context_selection_keys")) {
      auto selection = project.context_selections.find(*key.getAsString());
      if (selection == project.context_selections.end())
        return fail("INVALID_SELECTION", "unknown context selection key");
      selections.push_back(selection->second);
      request.selected_artifacts.push_back(selection->second.path);
    }
    auto snapshot = CaptureReviewSnapshot(request);
    if (!snapshot.ok())
      return fail("CAPTURE_FAILED", snapshot.status().message());
    auto budget = config_.structural;
    budget.max_entities = std::min(budget.max_entities, config_.max_examined);
    if (elapsed() >= remaining)
      return fail("LIMIT_EXCEEDED", "capture exhausted backend deadline");
    budget.wall_ms = std::min(budget.wall_ms, remaining - elapsed());
    auto index = BuildStructuralIndex(*snapshot, budget);
    if (!index.ok())
      return fail("UNSUPPORTED", index.status().message());
    for (const auto &frontier : index->coverage().frontier_refs)
      if (frontier.starts_with("entity-budget:") ||
          frontier.starts_with("tu-budget:"))
        return fail(
            "LIMIT_EXCEEDED",
            "structural work budget exhausted before snapshot publication");
    auto pending = std::make_unique<State>(
        State{*snapshot, *index, {}, index->coverage()});
    std::set<std::string> sources;
    for (const auto &cmd : snapshot->compiler_commands())
      sources.insert(cmd.file);
    // Headers/other source entities can supply provenance too.
    for (const auto &e : index->entities())
      sources.insert(e.path);
    for (const auto &path : sources)
      selections.push_back({path, "SOURCE"});
    for (const auto &s : selections) {
      auto text = snapshot->Read(s.path);
      if (!text.ok()) {
        pending->coverage.traversal = "INCOMPLETE";
        pending->coverage.frontier_refs.push_back(s.path);
        continue;
      }
      if (text->size() > config_.max_context_bytes) {
        pending->coverage.traversal = "INCOMPLETE";
        pending->coverage.reasons.push_back("source exceeds context window: " +
                                            s.path);
        continue;
      }
      auto context = snapshot->Context(s.path, s.kind, 0, text->size());
      if (!context.ok())
        return fail("UNSUPPORTED", context.status().message());
      pending->contexts.emplace(context->context_id.value(), *context);
      if (s.kind == "SOURCE") {
        auto indexed = index->Context(s.path, 0, text->size());
        if (!indexed.ok())
          return fail("UNSUPPORTED", indexed.status().message());
        pending->contexts.emplace(indexed->context_id.value(), *indexed);
        auto base = snapshot->ReadBase(s.path);
        if (!base.ok() || *base != *text) {
          // A full-file replacement is an exact, deterministic diff, not a
          // proximity-derived entity binding. Byte spans refer to this
          // artifact.
          std::string diff =
              "--- base/" + s.path + "\n+++ captured/" + s.path + "\n";
          auto append = [&](std::string_view content, char sign) {
            size_t at = 0;
            while (at < content.size()) {
              auto end = content.find('\n', at);
              if (end == std::string_view::npos)
                end = content.size();
              diff += sign;
              diff.append(content.substr(at, end - at));
              diff += '\n';
              at = end + 1;
            }
          };
          if (base.ok())
            append(*base, '-');
          append(*text, '+');
          if (diff.size() <= config_.max_context_bytes) {
            auto diff_id = MakeArtifactId<ContextTag>(
                O{{"snapshot_id", snapshot->binding().snapshot_id->value()},
                  {"path", s.path},
                  {"kind", "DIFF"},
                  {"excerpt", diff}});
            ContextArtifact d{*diff_id,
                              snapshot->binding(),
                              "DIFF",
                              s.path,
                              HashBytes(diff),
                              0,
                              diff.size(),
                              diff,
                              {},
                              "captured full-file replacement",
                              {}};
            pending->contexts.emplace(d.context_id.value(), std::move(d));
          } else {
            pending->coverage.traversal = "INCOMPLETE";
            pending->coverage.reasons.push_back(
                "diff exceeds context window: " + s.path);
          }
        }
      }
    }
    A refs;
    for (const auto &[key, value] : pending->contexts) {
      refs.emplace_back(key);
      for (const auto &entity : value.entity_bindings)
        if (entity.kind == "CALLSITE")
          pending->callsite_contexts[entity.entity_ref.ordinal] = key;
    }
    auto response = finish(O{{"binding", ToJson(snapshot->binding())},
                             {"context_refs", refs},
                             {"capabilities", Capabilities(config_, true)},
                             {"coverage", CoverageJson(pending->coverage)}});
    if (response.ok() && *DecodeV2(*response)->ok)
      state_ = std::move(pending);
    return response;
  }
  if (!state_ || !decoded->binding ||
      *decoded->binding != state_->snapshot.binding())
    return fail("BINDING_MISMATCH", "request does not match pinned snapshot");
  const auto binding = ToJson(state_->snapshot.binding());
  if (op == "native-evidence")
    return fail(
        "UNSUPPORTED",
        "no qualified native adapter configured; native binding required");
  if (op == "verify") {
    auto valid = ValidateVerificationRequest(q);
    if (!valid.ok())
      return fail("INVALID_ARGUMENT", valid.message());
    return fail("UNSUPPORTED", "no qualified production verifier configured");
  }
  if (op == "release") {
    auto response = finish(O{{"binding", binding}, {"released", true}});
    if (response.ok() && *DecodeV2(*response)->ok)
      state_.reset();
    return response;
  }
  if (op == "context" || op == "seed-candidates") {
    const auto &refs =
        *q.getArray(op == "context" ? "context_refs" : "changed_context_refs");
    if (refs.size() > config_.max_examined)
      return fail("LIMIT_EXCEEDED", "context handle work limit");
    std::vector<const ContextArtifact *> selected;
    for (const auto &ref : refs) {
      auto found = state_->contexts.find(*ref.getAsString());
      if (found == state_->contexts.end())
        return fail("INVALID_REFERENCE", "context was not advertised");
      selected.push_back(&found->second);
    }
    Usage usage;
    Coverage coverage = state_->coverage;
    coverage.examined = 0;
    if (op == "context") {
      uint64_t limit =
          std::min({Number(q, "max_bytes"), Number(q, "max_tokens"),
                    config_.max_context_bytes});
      A artifacts;
      for (const auto *c : selected) {
        if (c->excerpt.size() > limit - usage.output_bytes)
          return fail("LIMIT_EXCEEDED", "context exceeds byte/token allowance");
        usage.output_bytes += c->excerpt.size();
        artifacts.push_back(ContextJson(*c));
        ++coverage.examined;
      }
      usage.examined = coverage.examined;
      return finish(O{{"binding", binding},
                      {"artifacts", artifacts},
                      {"usage", UsageJson(usage)},
                      {"coverage", CoverageJson(coverage)}});
    }
    A candidates;
    std::set<uint32_t> emitted;
    uint64_t limit = std::min(Number(q, "limit"), uint64_t(16));
    std::set<std::string> changed_paths;
    for (const auto *context : selected) {
      auto before = state_->snapshot.ReadBase(context->path),
           after = state_->snapshot.Read(context->path);
      if (!before.ok() || !after.ok() || *before != *after)
        changed_paths.insert(context->path);
    }
    if (!changed_paths.empty())
      for (const auto &e : state_->index.entities()) {
        if (coverage.examined == config_.max_examined) {
          coverage.traversal = "INCOMPLETE";
          coverage.reasons.push_back("seed examined limit");
          break;
        }
        ++coverage.examined;
        if (!changed_paths.contains(e.path) || e.kind != "CALLSITE" ||
            !e.direct_call ||
            (e.name != "memcpy" && e.name != "__builtin_memcpy") ||
            !emitted.insert(e.ordinal).second)
          continue;
        auto source_ref = state_->callsite_contexts.find(e.ordinal);
        if (source_ref == state_->callsite_contexts.end())
          continue;
        const auto &source = state_->contexts.at(source_ref->second);
        if (candidates.size() >= limit) {
          coverage.traversal = "INCOMPLETE";
          coverage.reasons.push_back("seed candidate limit");
          continue;
        }
        candidates.emplace_back(
            O{{"binding", binding},
              {"origin", "DETERMINISTIC"},
              {"claim_kind", "copy_bound.v1"},
              {"anchor_refs",
               A{O{{"context_id", source.context_id.value()},
                   {"start", e.start},
                   {"end", e.end},
                   {"entity_ref", Ref({state_->index.id(), e.ordinal})}}}},
              {"contract_refs", A{}},
              {"question", "Is the consumed size within destination capacity "
                           "at this captured copy?"},
              {"assumptions", A{}}});
      }
    usage.examined = coverage.examined;
    return finish(O{{"binding", binding},
                    {"candidates", candidates},
                    {"coverage", CoverageJson(coverage)},
                    {"usage", UsageJson(usage)}});
  }
  if (op == "local-evidence") {
    const auto &r = *q.getObject("callsite_ref");
    auto index = ParseArtifactId<StructuralIndexTag>(Text(r, "index_id"));
    EntityRef target{*index, static_cast<uint32_t>(Number(r, "ordinal"))};
    if (target.index_id != state_->index.id() ||
        !state_->callsite_contexts.contains(target.ordinal))
      return fail("INVALID_REFERENCE", "callsite not advertised");
    if (Number(q, "max_examined") > config_.max_examined)
      return fail("LIMIT_EXCEEDED", "local examined allowance");
    std::vector<std::string> slots;
    for (const auto &s : *q.getArray("requested_slots"))
      slots.push_back(*s.getAsString());
    auto result = InvestigateLocal({state_->index, state_->snapshot.binding(),
                                    Text(q, "predicate_kind"), target, slots,
                                    Number(q, "max_examined")});
    if (!result.ok())
      return fail("INVALID_ARGUMENT", result.status().message());
    return finish(ToJson(*result));
  }
  return fail("UNSUPPORTED", "operation unavailable");
}
StatusOr<BackendConfig> DecodeBackendConfig(std::string_view bytes) {
  auto parsed = ParseStrictJson(bytes);
  if (!parsed.ok())
    return parsed.status();
  auto root = parsed->getAsObject();
  if (!root ||
      !Keys(*root,
            {"project_key", "project_root", "repository_id", "base_ref",
             "compile_commands_path", "selected_tracked", "selected_artifacts",
             "allowed_untracked", "context_selections", "programs", "limits"}))
    return Status::InvalidArgument(
        "closed backend operator configuration required");
  for (auto k : {"project_key", "project_root", "repository_id", "base_ref",
                 "compile_commands_path"})
    if (!root->getString(k))
      return Status::InvalidArgument("invalid config string");
  BackendConfig config;
  BackendProject project;
  project.snapshot.project_root = Text(*root, "project_root");
  project.snapshot.repository_id = Text(*root, "repository_id");
  project.snapshot.base_ref = Text(*root, "base_ref");
  project.snapshot.compile_commands_path = Text(*root, "compile_commands_path");
  for (auto k :
       {"selected_tracked", "selected_artifacts", "allowed_untracked"}) {
    auto a = root->getArray(k);
    if (!a)
      return Status::InvalidArgument("invalid config selection");
    for (const auto &v : *a) {
      auto s = v.getAsString();
      if (!s)
        return Status::InvalidArgument("invalid config path");
      if (std::string_view(k) == "selected_tracked")
        project.snapshot.selected_tracked.push_back(*s);
      else if (std::string_view(k) == "selected_artifacts")
        project.snapshot.selected_artifacts.push_back(*s);
      else
        project.allowed_untracked.insert(*s);
    }
  }
  auto contexts = root->getObject("context_selections"),
       programs = root->getObject("programs"),
       limits = root->getObject("limits");
  if (!contexts || !programs || !limits ||
      !Keys(*limits, {"wall_ms", "output_bytes", "examined", "context_bytes",
                      "request_bytes", "requests"}))
    return Status::InvalidArgument("invalid config maps/limits");
  for (auto &[key, value] : *contexts) {
    auto o = value.getAsObject();
    if (!o || !Keys(*o, {"path", "kind"}) || !o->getString("path") ||
        !o->getString("kind"))
      return Status::InvalidArgument("invalid context selection");
    project.context_selections.emplace(
        key, ContextSelection{Text(*o, "path"), Text(*o, "kind")});
  }
  for (auto &[key, value] : *programs) {
    auto o = value.getAsObject();
    if (!o || !Keys(*o, {"path", "target"}) || !o->getString("path") ||
        !o->getString("target"))
      return Status::InvalidArgument("invalid program selection");
    project.programs.emplace(key,
                             std::pair{Text(*o, "path"), Text(*o, "target")});
  }
  for (auto &[key, value] : *limits)
    if (!value.getAsUINT64() || !*value.getAsUINT64())
      return Status::InvalidArgument("positive limits required");
  config.wall_ms = Number(*limits, "wall_ms");
  config.max_output_bytes = Number(*limits, "output_bytes");
  config.max_examined = Number(*limits, "examined");
  config.max_context_bytes = Number(*limits, "context_bytes");
  config.max_request_bytes = Number(*limits, "request_bytes");
  config.max_requests = Number(*limits, "requests");
  if (config.max_request_bytes > 1024 * 1024 ||
      config.max_output_bytes > 1024 * 1024 ||
      config.max_context_bytes > 32768 || config.max_examined > 10000 ||
      config.wall_ms > 120000 || config.max_requests > 256)
    return Status::InvalidArgument(
        "config may lower hard protocol limits only");
  config.structural.wall_ms = std::min(config.wall_ms, uint64_t(60000));
  config.projects.emplace(Text(*root, "project_key"), std::move(project));
  return config;
}
} // namespace veritas::review
