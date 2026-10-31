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


"""Turn-scoped capabilities; persisted records contain hashes, never tokens."""
import hashlib
import hmac
import secrets


def mint(key, nonce):
    return hmac.new(key,("veritas.review.turn.v1:"+nonce).encode(),hashlib.sha256).hexdigest()


def digest(token):
    if type(token) is not str or len(token)!=64:raise ValueError("invalid turn capability")
    return hashlib.sha256(("veritas.review.capability.v1:"+token).encode()).hexdigest()


def validate(token, expected):
    if not hmac.compare_digest(digest(token),expected):raise ValueError("stale or consumed turn capability")


def rotate(data,key):
    data["nonce"]=secrets.token_hex(32)
    data["capability_hash"]=digest(mint(key,data["nonce"]))
