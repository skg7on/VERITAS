/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2019, The Souffle Developers. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file BrieIndex.cpp
 *
 * Interpreter index with generic interface.
 *
 ***********************************************************************/

#include "interpreter/Relation.h"
#include "ram/Relation.h"
#include "ram/analysis/Index.h"
#include "souffle/utility/MiscUtil.h"

namespace souffle::interpreter {

#define CREATE_BRIE_REL(Structure, Arity, ...)                                                               \
    case (Arity): {                                                                                          \
        return mk<Relation<Arity, interpreter::Brie>>(id.getAuxiliaryArity(), id.getName(), indexSelection); \
    }

Own<RelationWrapper> createBrieRelation(
        const ram::Relation& id, const ram::analysis::IndexCluster& /* orderSet */) {
    switch (id.getArity()) {
        FOR_EACH_BRIE(CREATE_BRIE_REL);

        default: fatal("Brie is not supported in the interpreter mode.");
    }
}

}  // namespace souffle::interpreter
