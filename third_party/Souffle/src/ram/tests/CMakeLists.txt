# Souffle - A Datalog Compiler
# Copyright (c) 2021 The Souffle Developers. All rights reserved
# Licensed under the Universal Permissive License v 1.0 as shown at:
# - https://opensource.org/licenses/UPL
# - <souffle root>/licenses/SOUFFLE-UPL.txt

include(SouffleTests)

souffle_add_binary_test(ram_condition_equal_clone_test ram)
souffle_add_binary_test(ram_statement_equal_clone_test ram)
souffle_add_binary_test(ram_expression_equal_clone_test ram)
souffle_add_binary_test(ram_relation_equal_clone_test ram)
souffle_add_binary_test(ram_type_conversion_test ram)
souffle_add_binary_test(matching_test ram)
souffle_add_binary_test(max_matching_test ram)
