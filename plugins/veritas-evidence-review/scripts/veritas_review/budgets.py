# Copyright 2026 VERITAS Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


"""Attempt-wide pre-dispatch reservations, including failed requests."""
from .validation import closed

class BudgetExhausted(ValueError):pass


def reserve(costs, policy, *, wall_ms, output_bytes, examined):
    for v in (wall_ms,output_bytes,examined):
        if type(v) is not int or v<=0:raise ValueError("positive reservation required")
    proposed={"backend_wall_ms":costs.get("backend_wall_ms",0)+wall_ms,
              "backend_output_bytes":costs.get("backend_output_bytes",0)+output_bytes,
              "backend_examined":costs.get("backend_examined",0)+examined}
    if wall_ms>policy.backend_call_ms or any(v>getattr(policy,k) for k,v in proposed.items()):raise BudgetExhausted("aggregate backend budget")
    costs.update(proposed)


def reconcile(costs, *, wall_ms, actual_wall_ms):
    if type(actual_wall_ms) is not int or actual_wall_ms<0:raise ValueError("invalid backend wall usage")
    costs["backend_wall_ms"]+=actual_wall_ms-wall_ms


def charge_model(costs,policy,usage):
    closed(usage,("input_tokens","output_tokens","output_limit"))
    if any(type(v) is not int or v<0 for v in usage.values()) or not 0<usage["output_limit"]<=policy.output_tokens or usage["output_tokens"]>usage["output_limit"]:
        raise ValueError("host output ceiling/usage required")
    if costs.get("model_turns",0)>=policy.max_turns:raise BudgetExhausted("model turn budget")
    if costs.get("model_input_tokens",0)+usage["input_tokens"]>policy.input_tokens:raise BudgetExhausted("model input budget")
    costs["model_turns"]=costs.get("model_turns",0)+1
    costs["model_input_tokens"]=costs.get("model_input_tokens",0)+usage["input_tokens"]
    costs["model_output_tokens"]=costs.get("model_output_tokens",0)+usage["output_tokens"]
