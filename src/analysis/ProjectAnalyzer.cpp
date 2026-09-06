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

#include "veritas/analysis/ProjectAnalyzer.h"

#include <filesystem>
#include <map>
#include <span>
#include <string>

#include "analysis/SouffleProvenance.h"
#include "analysis/cpg/CpgProjectionStage.h"
#include "analysis/pipeline/LocalAnalysisStage.h"
#include "analysis/svf/SvfAnalysisStage.h"
#include "analysis/svf/SvfConfig.h"
#include "analysis/svf/SvfMerge.h"
#include "cpg/CpgCanonicalizer.h"
#include "veritas/analysis/semantic/ModelBundle.h"
#include "veritas/build/ProjectInput.h"
#include "veritas/build/ProjectManifestLoader.h"
#include "veritas/core/Hash.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Version.h"
#include "veritas/facts/AnalysisFactBus.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/FactStore.h"
#include "veritas/summary/SummaryArtifact.h"
#include "veritas/wpa/CppConformanceExecutor.h"
#include "veritas/wpa/SccStateRepository.h"
#include "veritas/wpa/SouffleWpaExecutor.h"
#include "veritas/wpa/WpaOrchestrator.h"
#include "veritas/wpa/WpaRunRepository.h"

#include "ProjectAnalyzerInternal.h"
#include "ProjectPublicationCoordinator.h"

namespace veritas::analysis {

namespace {

// The versioned external-model bundle is a checked-in source resource. Its
// directory is baked in at configure time (VERITAS_LOGIC_MODELS_DIR) so the
// analyzer loads the exact bundle it was built against.
constexpr const char* kModelBundleRows =
    VERITAS_LOGIC_MODELS_DIR "/models.v1.tsv";
constexpr const char* kModelBundleManifest =
    VERITAS_LOGIC_MODELS_DIR "/models.v1.manifest";

}  // namespace

AnalysisConfig AnalysisConfig::Default() {
  auto svf_config = svf::SvfConfig::Default();
  return AnalysisConfig{
      .svf_soft_analysis_budget = svf_config.soft_analysis_budget,
      .svf_max_graph_nodes = svf_config.max_graph_nodes,
      .svf_max_emitted_facts = svf_config.max_emitted_facts,
      .svf_max_alias_pairs = svf_config.max_alias_pairs,
      .svf_field_sensitive = svf_config.field_sensitive,
      .wpa_engine = WpaEngineMode::kSouffle,
      .wpa_component_timeout = std::chrono::seconds(30),
      .wpa_component_memory_mb = 0,
      .wpa_threads = 1,
      .rule_bundle_version = "rules.v2",
      .model_bundle_version = "models.v1",
      .run_cpp_conformance_oracle = false,
  };
}

namespace {

svf::SvfConfig ToSvfConfig(const AnalysisConfig &config) {
  return svf::SvfConfig{
      .pointer_analysis = svf::PointerAnalysisKind::kAndersenWaveDiff,
      .soft_analysis_budget = config.svf_soft_analysis_budget,
      .max_graph_nodes = config.svf_max_graph_nodes,
      .max_emitted_facts = config.svf_max_emitted_facts,
      .max_alias_pairs = config.svf_max_alias_pairs,
      .field_sensitive = config.svf_field_sensitive,
  };
}

std::string HashString(std::string_view bytes) {
  return core::DigestToHex(core::ComputeSHA256(
      std::as_bytes(std::span(bytes.data(), bytes.size()))));
}

// The SVF configuration hash is the canonical analyzer-config string hashed.
std::string SvfConfigurationHash(const AnalysisConfig &config) {
  return HashString(ToSvfConfig(config).CanonicalAnalyzerConfig());
}

// The WPA configuration hash covers the component set, execution limits, and
// relation/rule/model bundle settings (design §4.1), length-prefixed so no
// choice of field contents can forge a boundary.
std::string WpaConfigurationHash(const AnalysisConfig &config) {
  auto append_field = [](std::string *out, std::string_view value) {
    out->append(std::to_string(value.size()));
    out->push_back(':');
    out->append(value);
  };
  std::string canonical = "veritas.wpa.config.v1";
  append_field(&canonical, "reachability");
  append_field(&canonical, "memory_effects");
  append_field(&canonical, std::to_string(config.wpa_component_timeout.count()));
  append_field(&canonical, std::to_string(config.wpa_component_memory_mb));
  append_field(&canonical, std::to_string(config.wpa_threads));
  append_field(&canonical, "relations.v2");
  append_field(&canonical, config.rule_bundle_version);
  append_field(&canonical, config.model_bundle_version);
  return HashString(canonical);
}

// The C++ conformance/emergency identity is derived from a canonical build
// fingerprint (design §4.3): the baked compiler/platform/build-type/flags, the
// VERITAS version and Git revision, and the rule-bundle version. It is distinct
// from the digest-derived Souffle identity and cannot reuse it.
std::string CppToolchainIdentity(const AnalysisConfig &config) {
  const auto version = veritas::GetVersion();
  std::string fingerprint = "veritas-cpp-build.v1;";
  fingerprint += VERITAS_CPP_BUILD_FINGERPRINT;
  fingerprint += ";version=";
  fingerprint += std::to_string(version.major) + "." +
                 std::to_string(version.minor) + "." +
                 std::to_string(version.patch);
  fingerprint += ";git_revision=" + version.git_revision;
  fingerprint += ";rule_bundle=" + config.rule_bundle_version;
  return "veritas-cpp-" + HashString(fingerprint);
}

// Compares two runs' completed components: every component key, canonical
// facts, ExternalHash, and FixpointHash must agree (design §4.3).
Status CompareCanonicalResults(const wpa::WpaRunResult &primary,
                               const wpa::WpaRunResult &conformance) {
  std::map<wpa::WpaComponentKey, const wpa::WpaComponentCompletion *>
      primary_map;
  for (const auto &completion : primary.completed_components) {
    primary_map[completion.key] = &completion;
  }
  std::map<wpa::WpaComponentKey, const wpa::WpaComponentCompletion *>
      conformance_map;
  for (const auto &completion : conformance.completed_components) {
    conformance_map[completion.key] = &completion;
  }
  if (primary_map.size() != conformance_map.size()) {
    return Status::FailedPrecondition("conformance component count differs");
  }
  for (const auto &[key, primary_completion] : primary_map) {
    const auto it = conformance_map.find(key);
    if (it == conformance_map.end()) {
      return Status::FailedPrecondition("conformance is missing a component");
    }
    const auto &c = it->second->result;
    const auto &p = primary_completion->result;
    if (p.external_hash != c.external_hash ||
        p.fixpoint_hash != c.fixpoint_hash) {
      return Status::FailedPrecondition(
          "conformance result hashes differ for component " +
          std::to_string(static_cast<int>(key.component)));
    }
    if (p.facts != c.facts) {
      return Status::FailedPrecondition("conformance canonical facts differ");
    }
  }
  return Status::Ok();
}

// Runs the WPA orchestrator over the just-published summaries and records the
// run identity, engine, and any degraded-mode or failure diagnostic.
Status RunWpa(const std::filesystem::path &output_root,
              const AnalysisConfig &config,
              const std::vector<summary::v2::FunctionSummary> &summaries,
              const build::AnalysisManifest &manifest,
              ProjectAnalysisResult *result) {
  auto revision = core::ParseStableId(manifest.context.revision_id);
  if (!revision.ok())
    return revision.status();
  auto build_variant = core::ParseStableId(manifest.context.build_variant_id);
  if (!build_variant.ok())
    return build_variant.status();

  facts::AnalysisRunDescriptor descriptor;
  descriptor.revision_id = *revision;
  descriptor.build_variant_id = *build_variant;
  descriptor.summary_schema_version = "summary.v2";
  descriptor.relation_schema_version = "relations.v2";
  descriptor.rule_bundle_version = config.rule_bundle_version;
  descriptor.model_bundle_version = config.model_bundle_version;
  descriptor.svf_configuration_hash = SvfConfigurationHash(config);
  descriptor.wpa_configuration_hash = WpaConfigurationHash(config);
  descriptor.engine = config.wpa_engine == WpaEngineMode::kSouffle
                          ? facts::EngineIdentity::kSouffle
                          : facts::EngineIdentity::kCppEmergency;
  std::string toolchain_identity;
  if (config.wpa_engine == WpaEngineMode::kSouffle) {
#ifdef VERITAS_SOUFFLE_PROVENANCE_PATH
    auto provenance = SouffleProvenance::Load(
        VERITAS_SOUFFLE_PROVENANCE_PATH, VERITAS_SOUFFLE_EXECUTABLE_PATH);
    if (!provenance.ok()) {
      return provenance.status();
    }
    toolchain_identity = provenance->toolchain_identity();
#else
    return Status::FailedPrecondition(
        "souffle WPA was requested but provenance is not built");
#endif
  } else {
    toolchain_identity = CppToolchainIdentity(config);
  }
  descriptor.engine_toolchain_identity = toolchain_identity;
  auto run = facts::MakeAnalysisRun(descriptor);
  if (!run.ok())
    return run.status();

  // One output root owns one SummaryDB: the WPA run state, SCC topology, facts,
  // and the persisted manifest context all live in <output_root>/metadata.db.
  // The SCC scheduler needs the revision/build-variant rows the publication
  // coordinator wrote there, so it must share this store, not a side database.
  auto repo = wpa::WpaRunRepository::Open(output_root);
  if (!repo.ok())
    return repo.status();

  wpa::SccStateRepository scc_state(repo->metadata_store());

  // The WPA consumes summaries as the variant type; the published drafts are
  // all v2, so wrap them.
  std::vector<summary::SummaryArtifact> artifacts;
  artifacts.reserve(summaries.size());
  for (const auto &summary : summaries) {
    artifacts.emplace_back(summary);
  }

  wpa::WpaExecutionLimits limits;
  limits.timeout = config.wpa_component_timeout;
  limits.memory_mb = config.wpa_component_memory_mb;
  limits.threads = config.wpa_threads;

  const std::array<wpa::WpaComponentKind, 4> components = {
      wpa::WpaComponentKind::kReachability,
      wpa::WpaComponentKind::kMemoryEffects,
      wpa::WpaComponentKind::kFlow,
      wpa::WpaComponentKind::kEffects};

  wpa::WpaRunRequest wpa_request;
  wpa_request.run = *run;
  wpa_request.summaries = artifacts;
  wpa_request.components = components;
  wpa_request.limits = limits;

  StatusOr<wpa::WpaRunResult> wpa_result = [&]() -> StatusOr<wpa::WpaRunResult> {
    if (config.wpa_engine == WpaEngineMode::kSouffle) {
#ifdef VERITAS_SOUFFLE_WORKER
      wpa::SouffleWpaExecutor executor(VERITAS_SOUFFLE_WORKER, toolchain_identity);
      wpa::WpaOrchestrator orchestrator(executor, *repo, &scc_state);
      return orchestrator.Run(wpa_request);
#else
      return Status::FailedPrecondition(
          "souffle WPA was requested but the worker is not built");
#endif
    }
    auto executor = wpa::CppConformanceExecutor::Create(
        facts::EngineIdentity::kCppEmergency, toolchain_identity);
    if (!executor.ok())
      return executor.status();
    wpa::WpaOrchestrator orchestrator(*executor, *repo, &scc_state);
    return orchestrator.Run(wpa_request);
  }();

  result->wpa_engine = config.wpa_engine;
  if (!wpa_result.ok()) {
    result->wpa_diagnostics = std::string(wpa_result.status().message());
    return wpa_result.status();
  }
  result->wpa_run_id = core::ToString(wpa_result->run.run_id);

  // Optional C++ conformance oracle: run a second, separately identified
  // kCppConformance execution over the same logical inputs and require the
  // canonical results to agree before any publication (design §4.3).
  if (config.run_cpp_conformance_oracle &&
      config.wpa_engine == WpaEngineMode::kSouffle) {
    facts::AnalysisRunDescriptor conformance_descriptor = descriptor;
    conformance_descriptor.engine = facts::EngineIdentity::kCppConformance;
    conformance_descriptor.engine_toolchain_identity =
        CppToolchainIdentity(config);
    auto conformance_run = facts::MakeAnalysisRun(conformance_descriptor);
    if (!conformance_run.ok()) {
      return conformance_run.status();
    }

    auto conformance_executor = wpa::CppConformanceExecutor::Create(
        facts::EngineIdentity::kCppConformance,
        conformance_descriptor.engine_toolchain_identity);
    if (!conformance_executor.ok()) {
      return conformance_executor.status();
    }

    wpa::WpaRunRequest conformance_request = wpa_request;
    conformance_request.run = *conformance_run;
    wpa::WpaOrchestrator conformance_orchestrator(*conformance_executor, *repo,
                                                  &scc_state);
    auto conformance_result =
        conformance_orchestrator.Run(conformance_request);
    if (!conformance_result.ok()) {
      repo->MarkIncomplete(wpa_result->run);
      result->wpa_diagnostics =
          std::string(conformance_result.status().message());
      return conformance_result.status();
    }
    if (Status mismatch =
            CompareCanonicalResults(*wpa_result, *conformance_result);
        !mismatch.ok()) {
      repo->MarkIncomplete(wpa_result->run);
      repo->MarkIncomplete(conformance_result->run);
      result->wpa_diagnostics = std::string(mismatch.message());
      return mismatch;
    }
  }

  // Build the canonical batch and publish it through the fact bus to a fact
  // store sink on the shared metadata database, so the run's facts become
  // explainable (design §3).
  auto batch = facts::MakeAnalysisFactBatch(*wpa_result);
  auto fact_store = facts::FactStore::Open(output_root);
  if (!fact_store.ok()) {
    return fact_store.status();
  }
  facts::AnalysisFactBus bus(*repo);
  bus.AddSink("fact-store", *fact_store);
  if (Status published = bus.Publish(batch); !published.ok()) {
    result->wpa_diagnostics = std::string(published.message());
    return published;
  }

  result->wpa_diagnostics =
      config.wpa_engine == WpaEngineMode::kCppEmergency
          ? "degraded: cpp-emergency WPA"
          : "";
  return Status::Ok();
}

} // namespace

// ProjectAnalyzer::Impl holds the implementation details
class ProjectAnalyzer::Impl {
public:
  Impl() : svf_stage_(std::make_unique<svf::SvfAnalysisStage>()) {}

  StatusOr<ProjectAnalysisResult>
  AnalyzeProject(const ProjectAnalysisRequest &request,
                 const AnalysisConfig &config) {
    // M1: resolve and load the project manifest.
    auto input = build::ResolveProjectInput(request);
    if (!input.ok())
      return input.status();
    auto manifest = build::LoadProjectManifest(*input);
    if (!manifest.ok())
      return manifest.status();

    // M4: build linked IR and extract local summary drafts.
    auto local = pipeline::RunLocalAnalysis(*manifest);
    if (!local.ok())
      return local.status();

    // M5: run the required SVF stage over the live ProgramIr.
    svf::AnalyzerRunContext run_context{
        .analyzer_run_id = manifest->context.revision_id,
        .llvm_toolchain_identity = "llvm",
        .program_module_hash = std::string(local->program_ir.module_hash()),
    };
    auto svf_result = svf_stage_->Analyze(local->program_ir, run_context,
                                          ToSvfConfig(config));
    if (!svf_result.ok())
      return svf_result.status();

    // Load the versioned external-model bundle; its hash and rows feed the
    // whole-program analysis, and modeled-function effects attach to the
    // modeled function's summary.
    auto model_bundle = semantic::ModelBundle::Load(kModelBundleRows,
                                                    kModelBundleManifest);
    if (!model_bundle.ok()) {
      return model_bundle.status();
    }

    // Merge SVF facts by stable owning function ID into the summary.v2 drafts.
    auto merged = svf::MergeSvfFactsV2(std::move(local->summary_drafts),
                                       svf_result->facts, *model_bundle);
    if (!merged.ok()) {
      return merged.status();
    }

    // M6: project the CPG from the live ProgramIr and completed summaries.
    auto revision_id = core::ParseStableId(manifest->context.revision_id);
    auto build_variant_id =
        core::ParseStableId(manifest->context.build_variant_id);
    if (!revision_id.ok())
      return revision_id.status();
    if (!build_variant_id.ok())
      return build_variant_id.status();
    auto graph = cpg::BuildThinCpg(cpg::CpgProjectionInput{
        .program_ir = local->program_ir,
        .completed_summaries = *merged,
        .revision_id = *revision_id,
        .build_variant_id = *build_variant_id,
    });
    if (!graph.ok())
      return graph.status();

    const std::string projection_id_str =
        core::ToString(::veritas::cpg::CpgCanonicalizer::ProjectionId(*graph));
    const std::size_t node_count = graph->nodes().size();
    const std::size_t edge_count = graph->edges().size();

    // M2 + M3 + M6: persist the context and publish summaries + CPG atomically.
    auto coordinator =
        ProjectPublicationCoordinator::Open(input->output_root.string());
    if (!coordinator.ok())
      return coordinator.status();
    auto persist = (*coordinator)->PersistManifestContext(*manifest);
    if (!persist.ok())
      return persist;
    const std::vector<summary::v2::FunctionSummary> summaries = *merged;
    auto published =
        (*coordinator)
            ->Publish(CompletedProjectAnalysis{std::move(*merged),
                                               std::move(*graph)});
    if (!published.ok())
      return published.status();

    ProjectAnalysisResult result;
    result.projection_id = projection_id_str;
    result.cpg_node_count = node_count;
    result.cpg_edge_count = edge_count;
    result.completion =
        svf_result->completion == svf::SvfMappingCompletion::kComplete
            ? AnalysisCompletion::kComplete
            : AnalysisCompletion::kCompleteWithUnknowns;
    result.program_context_id = manifest->context.revision_id;
    result.revision_id = manifest->context.revision_id;
    result.build_variant_id = manifest->context.build_variant_id;
    result.published_summary_ids.reserve(published->size());
    for (const auto &id : *published) {
      result.published_summary_ids.push_back(core::ToString(id));
    }
    result.unknowns.reserve(svf_result->facts.unknowns.size());
    for (const auto &unknown : svf_result->facts.unknowns) {
      result.unknowns.push_back(
          UnknownFact{unknown.scope, unknown.reason, unknown.provenance_ref});
    }

    // Run the recursive WPA over the just-published summaries.
    auto wpa_status =
        RunWpa(input->output_root, config, summaries, *manifest, &result);
    if (!wpa_status.ok()) {
      result.wpa_diagnostics = std::string(wpa_status.message());
      return wpa_status;
    }
    return result;
  }

  // Test seam for dependency injection
  void SetSvfStage(std::unique_ptr<svf::SvfAnalysisStage> stage) {
    svf_stage_ = std::move(stage);
  }

private:
  std::unique_ptr<svf::SvfAnalysisStage> svf_stage_;
};

ProjectAnalyzer::ProjectAnalyzer() : impl_(std::make_unique<Impl>()) {}

ProjectAnalyzer::~ProjectAnalyzer() = default;

ProjectAnalyzer::ProjectAnalyzer(ProjectAnalyzer &&) noexcept = default;

ProjectAnalyzer &
ProjectAnalyzer::operator=(ProjectAnalyzer &&) noexcept = default;

StatusOr<ProjectAnalysisResult>
ProjectAnalyzer::AnalyzeProject(const ProjectAnalysisRequest &request,
                                const AnalysisConfig &config) {
  return impl_->AnalyzeProject(request, config);
}

// Private constructor for test factory
ProjectAnalyzer::ProjectAnalyzer(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ProjectAnalyzer internal::ProjectAnalyzerTestFactory::Create(
    std::unique_ptr<svf::SvfAnalysisStage> stage) {
  auto impl = std::make_unique<ProjectAnalyzer::Impl>();
  impl->SetSvfStage(std::move(stage));
  return ProjectAnalyzer(std::move(impl));
}

} // namespace veritas::analysis
