/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2018 The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file SubproofGenerator.h
 *
 ***********************************************************************/

#pragma once

#include "ast2ram/provenance/ClauseTranslator.h"

namespace souffle::ast {
class Atom;
class Clause;
}  // namespace souffle::ast

namespace souffle::ram {
class Operation;
class Statement;
}  // namespace souffle::ram

namespace souffle::ast2ram {
class TranslatorContext;
}

namespace souffle::ast2ram::provenance {

class SubproofGenerator : public ast2ram::provenance::ClauseTranslator {
public:
    SubproofGenerator(const TranslatorContext& context);
    ~SubproofGenerator();

protected:
    Own<ram::Statement> createRamFactQuery(const ast::Clause& clause) const override;
    Own<ram::Statement> createRamRuleQuery(const ast::Clause& clause) override;
    Own<ram::Operation> addNegatedAtom(
            Own<ram::Operation> op, const ast::Clause& clause, const ast::Atom* atom) const override;
    Own<ram::Operation> generateReturnInstantiatedValues(const ast::Clause& clause) const;
    Own<ram::Operation> addBodyLiteralConstraints(
            const ast::Clause& clause, Own<ram::Operation> op) const override;
};
}  // namespace souffle::ast2ram::provenance
