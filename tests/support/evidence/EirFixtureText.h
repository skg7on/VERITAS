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

// EirFixtureText.h — EIR-T source shared by the parser test and the writer
// test.
//
// The constant lives in a header rather than in one test's translation unit
// because it is the contract between two tasks: M10C's parser reads this text,
// and M10C's canonical writer must reproduce it (up to the canonical ordering
// §19.1 fixes). A fixture owned by either test would let the two drift, and the
// round-trip property REP-001 would then be asserted against two different
// documents.
//
// The document is the formal specification's §15 example, completed for
// `eir.v1`: it carries a valid program context, a buffer-overflow claim against
// a `memcpy` call site, three facts (one derived, with provenance), one edge
// with a withheld expansion, a value-flow path, a constraint, an assumption, an
// open question with a suggested resolution, a proof obligation, a summary
// reference, exactly two dependencies, and exactly one omission. Every amended
// attribute Task 7a made writable appears at least once: the repeatable
// `analyzer` property with both of its optional arguments, `type_layout`,
// `analysis_run`, the entity and fact `stable_id`s, the fact `derived` flag,
// the unknown's `detail`, `blocking`, and `suggested_resolution`, the
// provenance's `rule`, `inputs`, `version`, `configuration`, `source_anchor`,
// and `analysis_run`, and the proof obligation's `result` and `producer`.
//
// The three attributes the language admits and the model cannot carry —
// `location` on a provenance record and `condition`, `transfer`, and `summary`
// on an edge — are deliberately absent: this document must parse.

#ifndef VERITAS_TESTING_EVIDENCE_EIR_FIXTURE_TEXT_H_
#define VERITAS_TESTING_EVIDENCE_EIR_FIXTURE_TEXT_H_

#include <string_view>

namespace veritas::testing {

// The run every provenance record and the case binding agree on.
inline constexpr std::string_view kOverflowRunId =
    "run:sha256:0d5c1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5";

// A complete, validator-clean EIR V0.1 case. Parsing it must succeed and
// produce a case whose `evidence_id` is the content address of its semantics
// alone.
inline constexpr std::string_view kOverflowEirText = R"EIR(// The DEM-001 overflow case, in the stabilized EIR-T 1.0 text format.
evidence Overflow_001 {
    schema = "eir.v1";
    level = l1;
    state = POSSIBLE_DEFECT;

    context {
        repository = "radio-stack";
        revision = "a87f03e";
        build_variant = "ARM64_RELEASE";
        target = "aarch64-unknown-linux-gnu";
        analyzer_configuration = "veritas.default";
        type_layout = "layout:aapcs64";
        analysis_run = "run:sha256:0d5c1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5";
        analyzer = veritas.clang("17.0.6", "veritas.default");
        analyzer = veritas.wpa("1.0");
    }

    entity E_len : value {
        stable_id = "valref:sha256:1111111111111111111111111111111111111111111111111111111111111111";
        origin = @E_packet_length;
    }

    entity E_packet_length : value {
        origin = "packet.length";
    }

    entity E_dst : memory_object {
        allocation_site = "decode_frame";
    }

    entity E_sink : callsite {
        function = "memcpy";
    }

    entity E_validate : function {
        origin = "validate_packet";
    }

    entity E_vendor_validate : function {
        origin = "vendor_validate";
    }

    claim C1 {
        kind = buffer_overflow;
        subject = @E_sink;
        predicate = @E_len > capacity(@E_dst);
        severity = high;
        description = "the destination buffer is smaller than the length copied";
    }

    fact F_range {
        predicate = range(@E_len, 0, 65535);
        epistemic = must;
        confidence = exact;
        source = clang;
        stable_id = "fact:sha256:2222222222222222222222222222222222222222222222222222222222222222";
    }

    fact F_capacity {
        predicate = capacity(@E_dst) == 2048;
        epistemic = must;
        confidence = high;
    }

    fact F_missing_check {
        predicate = not dominates(@E_validate, @E_sink);
        epistemic = inferred;
        confidence = medium;
        derived = true;
        provenance = @PR_check;
    }

    provenance PR_check {
        producer = veritas.wpa;
        rule = "dominates.absence";
        inputs = [$F_range, $F_capacity];
        source_anchor = "decode.cpp:281:9";
        version = "1.0";
        configuration = "veritas.default";
        analysis_run = "run:sha256:0d5c1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5";
    }

    provenance PR_result {
        producer = veritas.smt;
        rule = "range.satisfiability";
        inputs = [$F_missing_check];
        analysis_run = "run:sha256:0d5c1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5";
    }

    assumption A1 {
        predicate = @E_len >= 0;
        source = infer_contract(@E_vendor_validate);
        scope = decode_frame(@E_sink);
    }

    unknown U_vendor {
        property = not validated(@E_vendor_validate);
        reason = EXTERNAL_FUNCTION;
        detail = "vendor_validate has no body in this build variant";
        blocking = [@F_missing_check];
        suggested_resolution = infer_contract(@E_vendor_validate);
    }

    edge ED_flow {
        from = @E_len;
        to = @E_sink;
        kind = FLOWS_TO;
        epistemic = must;
        provenance = @PR_check;
        expandable = true;
        summarized_by = @S1;
    }

    path P1 value_flow {
        @E_len -> @E_sink;

        conditions {
            @E_len > 2048;
        }

        feasible = SAT;
        provenance = @PR_check;
    }

    constraint K1 {
        expr = @E_len <= 65535;
        scope = global;
        epistemic = must;
        provenance = @PR_check;
    }

    verify V1 {
        prove = @E_len <= capacity(@E_dst);
        using = [smt, symbolic];
        budget = 5000;
        status = PROVED;
        result = @PR_result;
        producer = veritas.smt;
    }

    summary S1 {
        function = @E_vendor_validate;
        summary_id = "summary:sha256:3333333333333333333333333333333333333333333333333333333333333333";
        components = [range, value_flow];
    }

    dependency D1 {
        kind = summary;
        stable_id = "summary:sha256:3333333333333333333333333333333333333333333333333333333333333333";
    }

    dependency D2 {
        kind = configuration;
        stable_id = "model:sha256:4444444444444444444444444444444444444444444444444444444444444444";
    }

    omission O1 {
        kind = analyzer_expansion;
        subject = @S1;
        reason = "the vendor callee is outside this build variant";
        expandable = true;
    }
}
)EIR";

// The same case with a display label, and without one. §3.1: the label carries
// no semantic content, so the two documents must produce the same case and the
// same `EvidenceID`. The writer task shares these so the falsification is
// asserted once, against one pair of documents.
inline constexpr std::string_view kOverflowEirTextWithoutLabel = R"EIR(evidence {
    schema = "eir.v1";
    level = l1;
    state = POSSIBLE_DEFECT;

    context {
        repository = "radio-stack";
        revision = "a87f03e";
        build_variant = "ARM64_RELEASE";
        target = "aarch64-unknown-linux-gnu";
        analyzer_configuration = "veritas.default";
        type_layout = "layout:aapcs64";
        analysis_run = "run:sha256:0d5c1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5";
        analyzer = veritas.clang("17.0.6", "veritas.default");
        analyzer = veritas.wpa("1.0");
    }

    entity E_len : value {
        stable_id = "valref:sha256:1111111111111111111111111111111111111111111111111111111111111111";
        origin = @E_packet_length;
    }

    entity E_packet_length : value {
        origin = "packet.length";
    }

    entity E_dst : memory_object {
        allocation_site = "decode_frame";
    }

    entity E_sink : callsite {
        function = "memcpy";
    }

    entity E_validate : function {
        origin = "validate_packet";
    }

    entity E_vendor_validate : function {
        origin = "vendor_validate";
    }

    claim C1 {
        kind = buffer_overflow;
        subject = @E_sink;
        predicate = @E_len > capacity(@E_dst);
        severity = high;
        description = "the destination buffer is smaller than the length copied";
    }

    fact F_range {
        predicate = range(@E_len, 0, 65535);
        epistemic = must;
        confidence = exact;
        source = clang;
        stable_id = "fact:sha256:2222222222222222222222222222222222222222222222222222222222222222";
    }

    fact F_capacity {
        predicate = capacity(@E_dst) == 2048;
        epistemic = must;
        confidence = high;
    }

    fact F_missing_check {
        predicate = not dominates(@E_validate, @E_sink);
        epistemic = inferred;
        confidence = medium;
        derived = true;
        provenance = @PR_check;
    }

    provenance PR_check {
        producer = veritas.wpa;
        rule = "dominates.absence";
        inputs = [$F_range, $F_capacity];
        source_anchor = "decode.cpp:281:9";
        version = "1.0";
        configuration = "veritas.default";
        analysis_run = "run:sha256:0d5c1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5";
    }

    provenance PR_result {
        producer = veritas.smt;
        rule = "range.satisfiability";
        inputs = [$F_missing_check];
        analysis_run = "run:sha256:0d5c1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5";
    }

    assumption A1 {
        predicate = @E_len >= 0;
        source = infer_contract(@E_vendor_validate);
        scope = decode_frame(@E_sink);
    }

    unknown U_vendor {
        property = not validated(@E_vendor_validate);
        reason = EXTERNAL_FUNCTION;
        detail = "vendor_validate has no body in this build variant";
        blocking = [@F_missing_check];
        suggested_resolution = infer_contract(@E_vendor_validate);
    }

    edge ED_flow {
        from = @E_len;
        to = @E_sink;
        kind = FLOWS_TO;
        epistemic = must;
        provenance = @PR_check;
        expandable = true;
        summarized_by = @S1;
    }

    path P1 value_flow {
        @E_len -> @E_sink;

        conditions {
            @E_len > 2048;
        }

        feasible = SAT;
        provenance = @PR_check;
    }

    constraint K1 {
        expr = @E_len <= 65535;
        scope = global;
        epistemic = must;
        provenance = @PR_check;
    }

    verify V1 {
        prove = @E_len <= capacity(@E_dst);
        using = [smt, symbolic];
        budget = 5000;
        status = PROVED;
        result = @PR_result;
        producer = veritas.smt;
    }

    summary S1 {
        function = @E_vendor_validate;
        summary_id = "summary:sha256:3333333333333333333333333333333333333333333333333333333333333333";
        components = [range, value_flow];
    }

    dependency D1 {
        kind = summary;
        stable_id = "summary:sha256:3333333333333333333333333333333333333333333333333333333333333333";
    }

    dependency D2 {
        kind = configuration;
        stable_id = "model:sha256:4444444444444444444444444444444444444444444444444444444444444444";
    }

    omission O1 {
        kind = analyzer_expansion;
        subject = @S1;
        reason = "the vendor callee is outside this build variant";
        expandable = true;
    }
}
)EIR";

}  // namespace veritas::testing

#endif  // VERITAS_TESTING_EVIDENCE_EIR_FIXTURE_TEXT_H_
