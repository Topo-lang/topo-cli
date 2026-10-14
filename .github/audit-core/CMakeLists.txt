cmake_minimum_required(VERSION 3.20)
project(TopoAuditCoreTests LANGUAGES CXX)
find_package(topo-core CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)
enable_testing()
if(NOT EXISTS "${TOPO_CORE_SOURCE}/test/unit/AnalysisProcessTest.cpp")
    message(FATAL_ERROR "TOPO_CORE_SOURCE must contain the audit and isolation regression tests")
endif()
add_executable(topo-audit-contract-tests "${TOPO_CORE_SOURCE}/test/unit/AuditTest.cpp")
target_link_libraries(topo-audit-contract-tests PRIVATE
    topo::core::TopoCheck topo::core::TopoDistribution GTest::gtest_main)
add_executable(topo-analysis-test-helper "${TOPO_CORE_SOURCE}/test/helpers/AnalysisProcessTestHelper.cpp")
target_link_libraries(topo-analysis-test-helper PRIVATE topo::core::TopoPlatform)
add_executable(topo-analysis-process-tests "${TOPO_CORE_SOURCE}/test/unit/AnalysisProcessTest.cpp")
target_link_libraries(topo-analysis-process-tests PRIVATE topo::core::TopoPlatform GTest::gtest_main)
target_compile_definitions(topo-analysis-process-tests PRIVATE
    TOPO_ANALYSIS_TEST_HELPER="$<TARGET_FILE:topo-analysis-test-helper>")
add_dependencies(topo-analysis-process-tests topo-analysis-test-helper)
foreach(target IN ITEMS topo-audit-contract-tests topo-analysis-test-helper topo-analysis-process-tests)
    target_compile_features(${target} PRIVATE cxx_std_17)
endforeach()
# One label keeps discovery portable across CMake versions that serialize
# semicolon-valued PROPERTIES differently. The CI runner selects exact suites.
gtest_discover_tests(topo-audit-contract-tests DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS audit TIMEOUT 120)
gtest_discover_tests(topo-analysis-process-tests DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS audit TIMEOUT 120)
