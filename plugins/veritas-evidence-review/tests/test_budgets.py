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


import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"scripts"))
from veritas_review.models import ReviewPolicy
from veritas_review.budgets import reserve, reconcile, charge_model, BudgetExhausted

class BudgetsTest(unittest.TestCase):
    def test_limits_positive_and_model_cannot_raise(self):
        for value in (0,-1,True):
            with self.assertRaises(ValueError):ReviewPolicy(max_turns=value)
        with self.assertRaises(ValueError):ReviewPolicy().lower(max_turns=9)

    def test_reserve_before_execution_and_error_keeps_charge(self):
        policy=ReviewPolicy(backend_wall_ms=100, backend_call_ms=60)
        costs={};reserve(costs,policy,wall_ms=60,output_bytes=20,examined=1)
        with self.assertRaises(BudgetExhausted):reserve(costs,policy,wall_ms=60,output_bytes=20,examined=1)
        reconcile(costs,wall_ms=60,actual_wall_ms=10)
        reserve(costs,policy,wall_ms=60,output_bytes=20,examined=1)
        self.assertEqual(costs["backend_wall_ms"],70)

    def test_ninth_turn_and_missing_model_usage(self):
        p=ReviewPolicy();costs={}
        for _ in range(8):charge_model(costs,p,{"input_tokens":1,"output_tokens":1,"output_limit":4096})
        with self.assertRaises(BudgetExhausted):charge_model(costs,p,{"input_tokens":1,"output_tokens":1,"output_limit":4096})
        with self.assertRaises(ValueError):charge_model({},p,None)
        with self.assertRaises(ValueError):charge_model({},p,{"input_tokens":1,"output_tokens":1,"output_limit":8192})
