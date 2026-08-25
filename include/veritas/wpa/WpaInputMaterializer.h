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

// WpaInputMaterializer.h — projects summaries into one per-SCC logical input.
//
// Materialization is the boundary between the durable Function Summary IR and
// the run-local relations.v2 execution projection. It is a pure function of
// the summaries, the successor support facts, the model bundle, and the
// semantic run configuration: the same inputs always produce the same rows,
// the same dense assignment, and the same LogicalInputHash.

#ifndef VERITAS_WPA_WPA_INPUT_MATERIALIZER_H_
#define VERITAS_WPA_WPA_INPUT_MATERIALIZER_H_

#include "veritas/core/Status.h"
#include "veritas/wpa/WpaComponent.h"

namespace veritas::wpa {

class WpaInputMaterializer {
 public:
  // Builds the logical component input for one SCC and component.
  //
  // Fails with InvalidArgument on an unparseable identity or an unknown
  // component, NotFound when scc_id is not a component of the supplied
  // summaries, and FailedPrecondition when a successor support fact does not
  // belong to the component's support relation.
  static StatusOr<WpaLogicalComponentInput> Build(
      const WpaMaterializationRequest& request);
};

}  // namespace veritas::wpa

#endif  // VERITAS_WPA_WPA_INPUT_MATERIALIZER_H_
