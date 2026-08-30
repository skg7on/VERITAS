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

// SouffleSemanticKeyFunctor.cpp — the six stateful semantic-key functors.
//
// This is the M8R.4 implementation of the ABI pinned in
// logic/common/semantic_key.dl. A witness names its result and input by
// semantic key, so a key built inside a generated Souffle program must be
// byte-identical to one built by the C++ canonicalizer (SemanticKeyCodec).
// Each functor therefore emits exactly one type-tagged, length-prefixed field
// through the codec; the generated program concatenates the resulting symbols
// with `cat` and only ever joins fields after a header.
//
// The six functors are the only sanctioned way to produce key material. The
// domain split is load-bearing: a stable id and a plain symbol carrying the
// same text, or an enum ordinal and a bare number, must never encode alike.
//
// Signature convention (Souffle 2.5 stateful functor ABI):
//   souffle::RamDomain name(souffle::SymbolTable*, souffle::RecordTable*, ...)
// Each is exported with C linkage as the exact name declared by semantic_key.dl.
//
// RamDomain is int32 in this build (SOUFFLE_DOMAIN_64BIT=OFF), while the codec
// encodes `number`/`unsigned`/`enum` as 64-bit decimal strings. The Datalog
// domains therefore travel as their 32-bit representations and are widened
// here the same way the canonicalizer widens the corresponding 64-bit cells,
// so the two sides agree for every value the relations actually carry.

#include <cstdint>
#include <string>

#include "souffle/SouffleInterface.h"

#include "veritas/facts/SemanticKeyCodec.h"

namespace {

// Decodes a symbol RamDomain to its string, copying the reference the symbol
// table hands back (the table may reuse its storage across encode calls).
std::string DecodeSymbol(souffle::SymbolTable* sym_table,
                         souffle::RamDomain symbol) {
  return sym_table->decode(symbol);
}

// Encodes a key string as a symbol RamDomain.
souffle::RamDomain EncodeSymbol(souffle::SymbolTable* sym_table,
                                std::string key) {
  return sym_table->encode(key);
}

}  // namespace

extern "C" {

souffle::RamDomain veritas_key_header_v1(souffle::SymbolTable* sym_table,
                                         souffle::RecordTable* /*record_table*/,
                                         souffle::RamDomain relation,
                                         souffle::RamDomain arity) {
  const std::string key = veritas::facts::EncodeKeyHeader(
      DecodeSymbol(sym_table, relation),
      static_cast<std::size_t>(static_cast<std::uint32_t>(arity)));
  return EncodeSymbol(sym_table, key);
}

souffle::RamDomain veritas_key_id_v1(souffle::SymbolTable* sym_table,
                                     souffle::RecordTable* /*record_table*/,
                                     souffle::RamDomain value) {
  return EncodeSymbol(sym_table,
                      veritas::facts::EncodeIdField(DecodeSymbol(sym_table, value)));
}

souffle::RamDomain veritas_key_field_symbol_v1(
    souffle::SymbolTable* sym_table, souffle::RecordTable* /*record_table*/,
    souffle::RamDomain value) {
  return EncodeSymbol(sym_table, veritas::facts::EncodeSymbolField(
                                     DecodeSymbol(sym_table, value)));
}

souffle::RamDomain veritas_key_field_number_v1(
    souffle::SymbolTable* sym_table, souffle::RecordTable* /*record_table*/,
    souffle::RamDomain value) {
  return EncodeSymbol(sym_table, veritas::facts::EncodeNumberField(
                                     static_cast<std::int64_t>(value)));
}

souffle::RamDomain veritas_key_field_unsigned_v1(
    souffle::SymbolTable* sym_table, souffle::RecordTable* /*record_table*/,
    souffle::RamDomain value) {
  return EncodeSymbol(sym_table,
                      veritas::facts::EncodeUnsignedField(static_cast<std::uint64_t>(
                          static_cast<std::uint32_t>(value))));
}

souffle::RamDomain veritas_key_field_enum_v1(
    souffle::SymbolTable* sym_table, souffle::RecordTable* /*record_table*/,
    souffle::RamDomain value) {
  return EncodeSymbol(sym_table, veritas::facts::EncodeEnumField(
                                     static_cast<std::uint64_t>(
                                         static_cast<std::uint32_t>(value))));
}

}  // extern "C"
