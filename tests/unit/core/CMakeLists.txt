# tests/unit/core/CMakeLists.txt
#
# Unit tests for veritas_core. GoogleTest was included from Dependencies.cmake
# (which pulls in the GoogleTest CMake helper module), so gtest_discover_tests
# is available here.

add_executable(StatusTest StatusTest.cpp)
target_link_libraries(StatusTest PRIVATE
  veritas_core
  GTest::gtest
  GTest::gtest_main
)
veritas_add_warnings(StatusTest)
gtest_discover_tests(StatusTest)

add_executable(VersionTest VersionTest.cpp)
target_link_libraries(VersionTest PRIVATE
  veritas_core
  GTest::gtest
  GTest::gtest_main
)
veritas_add_warnings(VersionTest)
gtest_discover_tests(VersionTest)
