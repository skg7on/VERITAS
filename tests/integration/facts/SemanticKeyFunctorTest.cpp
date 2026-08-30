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

// SemanticKeyFunctorTest.cpp — the six semantic-key functors produce the same
// bytes as SemanticKeyCodec. It links veritas-souffle-functors directly and
// calls the stateful functors against a concrete symbol table, so a mismatch
// is a hard failure that does not require a full worker round-trip to surface.

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "souffle/RecordTable.h"
#include "souffle/SymbolTable.h"
#include "souffle/datastructure/SymbolTableImpl.h"

#include "veritas/facts/SemanticKeyCodec.h"

namespace {

// The six stateful functors exported by veritas-souffle-functors, with the
// signature the generated programs call.
extern "C" {
souffle::RamDomain veritas_key_header_v1(souffle::SymbolTable*, souffle::RecordTable*,
                                         souffle::RamDomain, souffle::RamDomain);
souffle::RamDomain veritas_key_id_v1(souffle::SymbolTable*, souffle::RecordTable*,
                                     souffle::RamDomain);
souffle::RamDomain veritas_key_field_symbol_v1(souffle::SymbolTable*,
                                               souffle::RecordTable*,
                                               souffle::RamDomain);
souffle::RamDomain veritas_key_field_number_v1(souffle::SymbolTable*,
                                               souffle::RecordTable*,
                                               souffle::RamDomain);
souffle::RamDomain veritas_key_field_unsigned_v1(souffle::SymbolTable*,
                                                 souffle::RecordTable*,
                                                 souffle::RamDomain);
souffle::RamDomain veritas_key_field_enum_v1(souffle::SymbolTable*,
                                             souffle::RecordTable*,
                                             souffle::RamDomain);
}

std::string Decode(souffle::SymbolTable& sym_table, souffle::RamDomain symbol) {
  return sym_table.decode(symbol);
}

TEST(SemanticKeyFunctorTest, HeaderMatchesCodec) {
  souffle::SymbolTableImpl sym_table;
  for (const auto& [relation, arity] :
       {std::pair<std::string, int>{"DirectCall", 5},
        std::pair<std::string, int>{"ReachableCall", 3}}) {
    auto symbol = veritas_key_header_v1(
        &sym_table, nullptr, sym_table.encode(relation), arity);
    EXPECT_EQ(Decode(sym_table, symbol),
              veritas::facts::EncodeKeyHeader(relation, arity));
  }
}

TEST(SemanticKeyFunctorTest, FieldFunctorsMatchCodec) {
  souffle::SymbolTableImpl sym_table;

  // Id field, including a stable-id shaped cell with the codec's own
  // punctuation.
  EXPECT_EQ(Decode(sym_table,
                   veritas_key_id_v1(&sym_table, nullptr,
                                     sym_table.encode("memref:sha256:x"))),
            veritas::facts::EncodeIdField("memref:sha256:x"));

  // Symbol field, including a delimiter-like cell and an empty cell.
  EXPECT_EQ(Decode(sym_table,
                   veritas_key_field_symbol_v1(&sym_table, nullptr,
                                               sym_table.encode("a\tb"))),
            veritas::facts::EncodeSymbolField("a\tb"));
  EXPECT_EQ(Decode(sym_table,
                   veritas_key_field_symbol_v1(&sym_table, nullptr,
                                               sym_table.encode(""))),
            veritas::facts::EncodeSymbolField(""));

  // Number field widens int32 to int64 exactly as the codec encodes int64.
  EXPECT_EQ(Decode(sym_table,
                   veritas_key_field_number_v1(&sym_table, nullptr, -7)),
            veritas::facts::EncodeNumberField(-7));
  EXPECT_EQ(Decode(sym_table,
                   veritas_key_field_number_v1(&sym_table, nullptr, 0)),
            veritas::facts::EncodeNumberField(0));

  // Unsigned field widens uint32 to uint64.
  EXPECT_EQ(Decode(sym_table,
                   veritas_key_field_unsigned_v1(&sym_table, nullptr, 42)),
            veritas::facts::EncodeUnsignedField(42));

  // Enum field.
  EXPECT_EQ(Decode(sym_table,
                   veritas_key_field_enum_v1(&sym_table, nullptr, 3)),
            veritas::facts::EncodeEnumField(3));
}

// The domain split is load-bearing: a stable id and a plain symbol carrying the
// same text, and an enum ordinal and a bare number, must never encode alike.
TEST(SemanticKeyFunctorTest, DomainsDoNotCollide) {
  souffle::SymbolTableImpl sym_table;
  const auto id = Decode(sym_table, veritas_key_id_v1(&sym_table, nullptr,
                                                      sym_table.encode("7")));
  const auto number = Decode(
      sym_table, veritas_key_field_number_v1(&sym_table, nullptr, 7));
  const auto symbol = Decode(sym_table, veritas_key_field_symbol_v1(
                                            &sym_table, nullptr,
                                            sym_table.encode("7")));
  const auto enum_field = Decode(
      sym_table, veritas_key_field_enum_v1(&sym_table, nullptr, 7));

  EXPECT_NE(id, symbol);       // id vs symbol with the same text
  EXPECT_NE(number, enum_field);  // number vs enum with the same ordinal
}

}  // namespace
