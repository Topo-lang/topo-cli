cmake_minimum_required(VERSION 3.20)
project(TopoAuditExtractor LANGUAGES C CXX)
include(GNUInstallDirs)
find_package(topo-core CONFIG REQUIRED)
find_package(topo-lang CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(LLVM CONFIG REQUIRED)
find_package(Clang CONFIG REQUIRED)
if(NOT EXISTS "${TOPO_CPP_SOURCE}/topo-check/extractor/CMakeLists.txt")
    message(FATAL_ERROR "TOPO_CPP_SOURCE must name the checked-out topo-lang-cpp source")
endif()
include("${TOPO_CPP_SOURCE}/cmake/TopoLangCppCompilerFlags.cmake")
# Exercise the maintained extractor target without enabling unrelated LLVM
# optimization/debug components or requiring a topo-llvm installation.
set(TOPO_ENABLE_LLVM ON)
add_subdirectory("${TOPO_CPP_SOURCE}/topo-check/extractor" extractor)

# Compile the package's maintained semantic/driver tests against the installed
# analysis library and the exact extractor built above.
find_package(topo-lang-cpp CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)
enable_testing()
add_executable(topo-cpp-preprocessor-output-fixture
    "${TOPO_CPP_SOURCE}/test/unit/CppPreprocessorOutputFixture.cpp")
target_compile_features(topo-cpp-preprocessor-output-fixture PRIVATE cxx_std_17)
add_executable(topo-cpp-audit-semantic-tests
    "${TOPO_CPP_SOURCE}/test/unit/CppAuditTest.cpp"
    "${TOPO_CPP_SOURCE}/test/unit/CppAuditDriverTest.cpp"
    "${TOPO_CPP_SOURCE}/topo-check/extractor/CppAuditDriver.cpp"
    "${TOPO_CPP_SOURCE}/topo-check/extractor/CppAuditExtractor.cpp")
target_compile_features(topo-cpp-audit-semantic-tests PRIVATE cxx_std_17)
target_compile_definitions(topo-cpp-audit-semantic-tests PRIVATE
    TOPO_CPP_AUDIT_LIBCLANG=1
    TOPO_CPP_PREPROCESSOR_OUTPUT_FIXTURE="$<TARGET_FILE:topo-cpp-preprocessor-output-fixture>")
target_include_directories(topo-cpp-audit-semantic-tests PRIVATE
    "${TOPO_CPP_SOURCE}/topo-check" "${TOPO_CPP_SOURCE}/topo-check/extractor")
target_include_directories(topo-cpp-audit-semantic-tests SYSTEM PRIVATE
    ${CLANG_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_link_libraries(topo-cpp-audit-semantic-tests PRIVATE
    topo::lang-cpp::TopoCppAnalysis topo::core::TopoDistribution libclang clangDriver GTest::gtest_main)
add_dependencies(topo-cpp-audit-semantic-tests
    topo-extract-cpp topo-cpp-preprocessor-output-fixture)
add_custom_command(TARGET topo-cpp-audit-semantic-tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:topo-extract-cpp> $<TARGET_FILE_DIR:topo-cpp-audit-semantic-tests>)
gtest_discover_tests(topo-cpp-audit-semantic-tests TEST_PREFIX "cpp-audit-semantic."
    DISCOVERY_MODE PRE_TEST PROPERTIES LABELS audit TIMEOUT 120)
