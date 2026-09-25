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

#ifndef VERITAS_CORE_ALLOCATOR_RELIEF_H_
#define VERITAS_CORE_ALLOCATOR_RELIEF_H_

namespace veritas::core {

// Asks the platform allocator to return memory it is holding in fully-emptied
// arenas to the operating system.
//
// Best effort and advisory by construction: on a platform that offers no such
// facility the call does nothing, and where one exists the amount returned is
// whatever that platform can spare. It never fails, has no return value, and
// does not observe or alter any program state, so it is valid to call at any
// point in a run, including on a heap with nothing to release.
//
// Callers use it at phase boundaries where a large working set has just become
// dead and the process would otherwise hold those pages for the rest of the
// run. The intended observable effect is a step down in resident set size, not
// a change in results.
void ReleaseFreedMemory();

}  // namespace veritas::core

#endif  // VERITAS_CORE_ALLOCATOR_RELIEF_H_
