// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The VERITAS Authors.
#include "veritas/core/Status.h"

// All Status member functions are defined inline in the header. This
// translation unit exists so veritas_core has at least one source file that
// references the Status API, which anchors debug info and forces the
// compiler to emit key/vtable-like artifacts (should any be added later)
// in a single object file rather than duplicating them across every TU.

namespace veritas {

// Intentionally empty — see comment above.

}  // namespace veritas
