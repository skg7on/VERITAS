# Souffle - A Datalog Compiler
# Copyright (c) 2021 The Souffle Developers. All rights reserved
# Licensed under the Universal Permissive License v 1.0 as shown at:
# - https://opensource.org/licenses/UPL
# - <souffle root>/licenses/SOUFFLE-UPL.txt

include(SouffleTests)

souffle_add_binary_test(interpreter_relation_test interpreter)
souffle_add_binary_test(ram_arithmetic_test interpreter)
souffle_add_binary_test(ram_relation_test interpreter)
