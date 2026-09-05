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

// SouffleRunner.h — the C ABI into the compiled Souffle rule bundles.
//
// The Souffle programs are generated with __EMBEDDED_SOUFFLE__ and linked into
// a shared library that is compiled with RTTI and exceptions enabled (Souffle
// requires both). This C ABI is the boundary to the rest of VERITAS, which is
// compiled with -fno-rtti -fno-exceptions: the function catches std::exception
// internally and never lets a Souffle type or exception cross the boundary.

#ifndef VERITAS_WPA_SOUFFLE_RUNNER_H_
#define VERITAS_WPA_SOUFFLE_RUNNER_H_

#ifdef __cplusplus
extern "C" {
#endif

// Runs the named component's compiled Souffle program over `input_dir` (the
// -F relation inputs) and writes derived relations and witnesses to
// `output_dir` (the -D destination). `component` is "reachability" or
// "memory-effects". Returns 0 on success and a non-zero status otherwise.
int veritas_souffle_run(const char* component, const char* input_dir,
                        const char* output_dir, unsigned jobs);

#ifdef __cplusplus
}
#endif

#endif  // VERITAS_WPA_SOUFFLE_RUNNER_H_
