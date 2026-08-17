/* Copyright 2026 VERITAS Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Fixture translation unit for the mixed-compilers determinism regression
 * test. The content is shared byte-for-byte between the mixed_compilers_ab
 * and mixed_compilers_ba fixtures so that hashing sees a single project.
 */

int b_function(int value) { return value * 2; }
