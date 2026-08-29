/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file ast_parser_utils_test.cpp
 *
 * Tests souffle's parser utils.
 *
 ***********************************************************************/

#include "tests/test.h"

#include "ast/Atom.h"
#include "ast/Clause.h"
#include "ast/Node.h"
#include "parser/ParserUtils.h"
#include "souffle/utility/ContainerUtil.h"
#include "souffle/utility/StringUtil.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace souffle::ast {

namespace test {

TEST(RuleBody, Basic) {
    RuleBody body;

    // start with an A
    auto a = RuleBody::atom(mk<Atom>("A"));
    EXPECT_EQ("A()", toString(a));

    a.conjunct(RuleBody::atom(mk<Atom>("B")));
    EXPECT_EQ("A(),B()", toString(a));

    a.disjunct(RuleBody::atom(mk<Atom>("C")));
    EXPECT_EQ("A(),B();C()", toString(a));
}

TEST(RuleBody, Negation) {
    RuleBody body = RuleBody::getTrue();

    RuleBody AB = RuleBody::getTrue();
    AB.conjunct(RuleBody::atom(mk<Atom>("A")));
    AB.conjunct(RuleBody::atom(mk<Atom>("B")));
    EXPECT_EQ("A(),B()", toString(AB));

    RuleBody CD = RuleBody::getTrue();
    CD.conjunct(RuleBody::atom(mk<Atom>("C")));
    CD.conjunct(RuleBody::atom(mk<Atom>("D")));
    EXPECT_EQ("C(),D()", toString(CD));

    RuleBody EF = RuleBody::getTrue();
    EF.conjunct(RuleBody::atom(mk<Atom>("E")));
    EF.conjunct(RuleBody::atom(mk<Atom>("F")));
    EXPECT_EQ("E(),F()", toString(EF));

    RuleBody full = RuleBody::getFalse();
    full.disjunct(std::move(AB));
    full.disjunct(std::move(CD));
    full.disjunct(std::move(EF));
    EXPECT_EQ("A(),B();C(),D();E(),F()", toString(full));

    full = full.negated();
    EXPECT_EQ(
            "!A(),!C(),!E();!A(),!C(),!F();!A(),!D(),!E();!A(),!D(),!F();!B(),!C(),!E();!B(),!C(),!F();!B(),!"
            "D(),!E();!B(),!D(),!F()",
            toString(full));

    full = full.negated();
    EXPECT_EQ("A(),B();C(),D();E(),F()", toString(full));
}

TEST(RuleBody, ClauseBodyExtraction) {
    RuleBody body = RuleBody::getTrue();

    RuleBody AB = RuleBody::getTrue();
    AB.conjunct(RuleBody::atom(mk<Atom>("A")));
    AB.conjunct(RuleBody::atom(mk<Atom>("B")));
    EXPECT_EQ("A(),B()", toString(AB));

    RuleBody CD = RuleBody::getTrue();
    CD.conjunct(RuleBody::atom(mk<Atom>("C")));
    CD.conjunct(RuleBody::atom(mk<Atom>("D")));
    EXPECT_EQ("C(),D()", toString(CD));

    RuleBody EF = RuleBody::getTrue();
    EF.conjunct(RuleBody::atom(mk<Atom>("E")));
    EF.conjunct(RuleBody::atom(mk<Atom>("F")));
    EXPECT_EQ("E(),F()", toString(EF));

    RuleBody full = RuleBody::getFalse();
    full.disjunct(std::move(AB));
    full.disjunct(std::move(CD));
    full.disjunct(std::move(EF));
    EXPECT_EQ("A(),B();C(),D();E(),F()", toString(full));

    // extract the clause
    auto list = full.toClauseBodies();
    EXPECT_EQ(3, list.size());

    EXPECT_EQ("*() :- \n   A(),\n   B().", toString(*list[0]));
    EXPECT_EQ("*() :- \n   C(),\n   D().", toString(*list[1]));
    EXPECT_EQ("*() :- \n   E(),\n   F().", toString(*list[2]));
}

}  // end namespace test
}  // namespace souffle::ast
