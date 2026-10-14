cmake_minimum_required(VERSION 3.20)

project(topo-cli
    VERSION 0.1.0
    DESCRIPTION "Topo plugin-dispatch CLI tools — topo-check / topo-build / topo-init / topo-transpile / topo-profile"
    HOMEPAGE_URL "https://github.com/topo-lang/topo-cli"
    # C is enabled alongside CXX so transitive check_include_file() probes work
    # (e.g. LLVM's libedit/histedit.h check, reached via find_package(LLVM) on
    # macOS where readline is absent). Without C enabled those C-language checks
    # abort configure with "Unknown extension .c". topo-cli has no .c sources.
    LANGUAGES C CXX
)

# topo-cli — the top-level consumer that hosts the five plugin-dispatch
# CLI tools (topo-check, topo-build, topo-init, topo-transpile,
# topo-profile). Each plugin-dispatching CLI links the per-host
# Topo<Lang>Plugin aggregator targets from topo-lang-* packages; the
# topo-profile CLI is plugin-free (consumes only topo-core profile libs
# + topo-v8 SourceMapResolver).
#
# Plugin availability is auto-detected — TOPO_CLI_ENABLE_<LANG>_PLUGIN
# defaults ON, but if find_package(topo-lang-<lang>) is missing OR the
# package does not export the Topo<Lang>Plugin aggregator (e.g. the
# LLVM-scope-gated TopoCppPlugin / TopoRustPlugin), the option flips
# OFF and the corresponding registerXxxPlugin() call is dropped via
# TOPO_CLI_WITH_<LANG>_PLUGIN compile definitions. Same shape as
# topo-lsp's plugin gating.
#
# TopoCheckRunner is shipped as an exported library
# (topo::cli::TopoCheckRunner) so downstream checker-e2e tests in
# topo-lang-* packages can link it directly without reaching into
# topo-cli's tools/ source tree.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(TopoCliCompilerFlags)

# ── Upstream Topo packages ──────────────────────────────
# Guarded find_package: a parent aggregator build (e.g. a meta-repo
# add_subdirectory chain) may have already brought these in via direct
# source inclusion, in which case the namespaced targets are live and
# re-running find_package would either fail (no installed Config) or
# clobber the source-tree targets. Sentinel-target skip is the
# standalone-vs-aggregated dual-mode pattern.
if(NOT TARGET topo::core::TopoBasic)
    find_package(topo-core CONFIG REQUIRED)
endif()
if(NOT TARGET topo::lang::TopoLang)
    find_package(topo-lang CONFIG REQUIRED)
endif()

# topo-v8 is only needed by topo-profile (SourceMapResolver) — make it
# optional so a build without TS / V8-stack consumers can still produce
# the other four CLIs.
option(TOPO_CLI_ENABLE_PROFILE "Build the topo-profile CLI (requires topo-v8)" ON)
if(TOPO_CLI_ENABLE_PROFILE)
    if(NOT TARGET topo::v8::Topov8Placeholder)
        find_package(topo-v8 CONFIG QUIET)
    endif()
    if(NOT TARGET topo::v8::Topov8Placeholder AND NOT topo-v8_FOUND)
        message(STATUS "topo-cli: topo-v8 not found; disabling topo-profile CLI (SourceMapResolver lives there)")
        set(TOPO_CLI_ENABLE_PROFILE OFF CACHE BOOL "" FORCE)
    endif()
endif()

# ── External dependencies ───────────────────────────────
find_package(nlohmann_json CONFIG REQUIRED)
# toml++ as header-only (avoids the RTTI conflict the monorepo notes)
find_path(TOMLPLUSPLUS_INCLUDE_DIR toml++/toml.hpp REQUIRED)

# ── Plugin auto-detection ───────────────────────────────
option(TOPO_CLI_ENABLE_CPP_PLUGIN        "Link topo::lang-cpp::TopoCppPlugin"               ON)
option(TOPO_CLI_ENABLE_RUST_PLUGIN       "Link topo::lang-rust::TopoRustPlugin"             ON)
option(TOPO_CLI_ENABLE_JAVA_PLUGIN       "Link topo::lang-java::TopoJavaPlugin"             ON)
option(TOPO_CLI_ENABLE_PYTHON_PLUGIN     "Link topo::lang-python::TopoPythonPlugin"         ON)
option(TOPO_CLI_ENABLE_TYPESCRIPT_PLUGIN "Link topo::lang-typescript::TopoTypeScriptPlugin" ON)

# Each plugin entry: <lang-lower>:<LANG_UPPER>:<LangPascal>
# capitalization matches the Topo<Pascal>Plugin target name (e.g.
# TopoCppPlugin not TopoCPPPlugin, TopoTypeScriptPlugin not
# TopoTYPESCRIPTPlugin).
set(_TOPO_CLI_PLUGIN_LIST
    "cpp:CPP:Cpp"
    "rust:RUST:Rust"
    "java:JAVA:Java"
    "python:PYTHON:Python"
    "typescript:TYPESCRIPT:TypeScript"
)

set(_TOPO_CLI_RESOLVED_PLUGINS "")
foreach(entry IN LISTS _TOPO_CLI_PLUGIN_LIST)
    string(REPLACE ":" ";" _parts "${entry}")
    list(GET _parts 0 _lang_lower)
    list(GET _parts 1 _lang_upper)
    list(GET _parts 2 _lang_pascal)

    if(NOT TOPO_CLI_ENABLE_${_lang_upper}_PLUGIN)
        message(STATUS "topo-cli: ${_lang_lower} plugin disabled by option")
        continue()
    endif()

    set(_plugin_target "topo::lang-${_lang_lower}::Topo${_lang_pascal}Plugin")
    # Sentinel-guard: skip find_package when the plugin aggregator is
    # already a live target (aggregated build via add_subdirectory).
    if(NOT TARGET ${_plugin_target})
        find_package(topo-lang-${_lang_lower} CONFIG QUIET)
    endif()
    if(NOT TARGET ${_plugin_target} AND NOT topo-lang-${_lang_lower}_FOUND)
        message(STATUS "topo-cli: topo-lang-${_lang_lower} not found; disabling ${_lang_lower} plugin")
        set(TOPO_CLI_ENABLE_${_lang_upper}_PLUGIN OFF CACHE BOOL "" FORCE)
        continue()
    endif()

    if(NOT TARGET ${_plugin_target})
        message(STATUS "topo-cli: topo-lang-${_lang_lower} found but ${_plugin_target} target missing (likely backend-scope-gated OFF upstream); disabling ${_lang_lower} plugin")
        set(TOPO_CLI_ENABLE_${_lang_upper}_PLUGIN OFF CACHE BOOL "" FORCE)
        continue()
    endif()

    message(STATUS "topo-cli: ${_lang_lower} plugin will be linked (${_plugin_target})")
    list(APPEND _TOPO_CLI_RESOLVED_PLUGINS "${_lang_lower}:${_lang_upper}:${_lang_pascal}")
endforeach()

if(NOT _TOPO_CLI_RESOLVED_PLUGINS)
    message(WARNING "topo-cli: no language plugins resolved; plugin-dispatching CLIs will only error out at runtime when asked for a specific host language")
endif()

# Pre-compute the link targets + compile-defs lists used by the four
# plugin-dispatching CLI subdirectories so each one doesn't have to
# repeat the auto-detect pattern. Each subdirectory CMakeLists reads
# _TOPO_CLI_PLUGIN_LINK_TARGETS and _TOPO_CLI_PLUGIN_COMPILE_DEFS.
set(_TOPO_CLI_PLUGIN_LINK_TARGETS "")
set(_TOPO_CLI_PLUGIN_COMPILE_DEFS "")
foreach(entry IN LISTS _TOPO_CLI_RESOLVED_PLUGINS)
    string(REPLACE ":" ";" _parts "${entry}")
    list(GET _parts 0 _lang_lower)
    list(GET _parts 1 _lang_upper)
    list(GET _parts 2 _lang_pascal)
    list(APPEND _TOPO_CLI_PLUGIN_LINK_TARGETS
        "topo::lang-${_lang_lower}::Topo${_lang_pascal}Plugin")
    list(APPEND _TOPO_CLI_PLUGIN_COMPILE_DEFS
        "TOPO_CLI_WITH_${_lang_upper}_PLUGIN=1")
endforeach()

# ── Subdirectories ─────────────────────────────────────
add_subdirectory(tools/topo-check)
add_subdirectory(tools/topo-build)
add_subdirectory(tools/topo-init)
add_subdirectory(tools/topo-transpile)
if(TOPO_CLI_ENABLE_PROFILE)
    add_subdirectory(tools/topo-profile)
endif()

# ── Install / export Config.cmake ──────────────────────
# Each tools/<x>/CMakeLists.txt does its own install(TARGETS ... EXPORT
# topo-cliTargets). We aggregate them all here.

install(EXPORT topo-cliTargets
    FILE topo-cliTargets.cmake
    NAMESPACE topo::cli::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-cli
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/topo-cliConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/topo-cliConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-cli
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/topo-cliConfigVersion.cmake
    VERSION       ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/topo-cliConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/topo-cliConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-cli
)

# ── Tests ─────────────────────────────────────────────
# Trust-store and policy tests live here; language end-to-end fixtures
# also exercise the exported runner from the per-language packages.
option(TOPO_CLI_BUILD_TESTS "Build topo-cli tests" OFF)
if(TOPO_CLI_BUILD_TESTS AND (PROJECT_IS_TOP_LEVEL OR TOPO_META_DRIVING_TESTS))
    enable_testing()
    find_package(GTest REQUIRED)
    include(GoogleTest)
    add_executable(topo-audit-support-tests test/audit/AuditSupportTest.cpp)
    target_link_libraries(topo-audit-support-tests PRIVATE TopoCheckRunner GTest::gtest_main)
    gtest_discover_tests(topo-audit-support-tests DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS audit)
endif()

# Tests can consume installed language tools as well as in-tree targets.
set(TOPO_AUDIT_CPP_EXTRACTOR "" CACHE FILEPATH "Installed topo-extract-cpp for audit tests")
set(TOPO_AUDIT_JAVA_EXTRACTOR_DIR "" CACHE PATH "Directory containing the Java extractor launcher and runtime descriptor")
set(TOPO_AUDIT_JAVA_BACKEND "" CACHE FILEPATH "Installed topo-build-jvm-java for build audit tests")
option(TOPO_CLI_REQUIRE_AUDIT_TESTS "Require the complete macOS C++/Java audit regression suite" OFF)
if(TOPO_CLI_REQUIRE_AUDIT_TESTS AND (NOT TOPO_CLI_BUILD_TESTS OR NOT APPLE))
    message(FATAL_ERROR "TOPO_CLI_REQUIRE_AUDIT_TESTS requires TOPO_CLI_BUILD_TESTS=ON on macOS")
endif()
set(_topo_audit_cpp_dir "")
if(TARGET topo-extract-cpp)
    set(_topo_audit_cpp_dir "$<TARGET_FILE_DIR:topo-extract-cpp>")
elseif(TOPO_AUDIT_CPP_EXTRACTOR AND EXISTS "${TOPO_AUDIT_CPP_EXTRACTOR}")
    get_filename_component(_topo_audit_cpp_dir "${TOPO_AUDIT_CPP_EXTRACTOR}" DIRECTORY)
elseif(TARGET topo::lang-cpp::topo-extract-cpp)
    set(_topo_audit_cpp_dir "$<TARGET_FILE_DIR:topo::lang-cpp::topo-extract-cpp>")
endif()

# Manual-file boundaries must preserve physical ownership across translation units.
if(TOPO_CLI_BUILD_TESTS AND APPLE AND _topo_audit_cpp_dir AND TOPO_CLI_ENABLE_CPP_PLUGIN)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    find_program(TOPO_AUDIT_TEST_CLANG NAMES clang++ HINTS "${LLVM_TOOLS_BINARY_DIR}")
    if(Python3_Interpreter_FOUND AND TOPO_AUDIT_TEST_CLANG)
        add_test(NAME e2e.cli.cpp_audit
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/smoke/cpp_audit.py"
                --check "$<TARGET_FILE:topo-check>"
                --extractor-dir "${_topo_audit_cpp_dir}"
                --compiler "${TOPO_AUDIT_TEST_CLANG}")
        # The lifecycle repeatedly hashes the full loaded toolchain; unoptimized
        # Debug SHA-256 costs more than each bounded extractor invocation.
        set_tests_properties(e2e.cli.cpp_audit PROPERTIES LABELS "e2e;cpp;cli;audit" TIMEOUT 600)
        add_test(NAME e2e.cli.cpp_audit_headers
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/smoke/cpp_audit_headers.py"
                --check "$<TARGET_FILE:topo-check>"
                --extractor-dir "${_topo_audit_cpp_dir}"
                --compiler "${TOPO_AUDIT_TEST_CLANG}")
        set_tests_properties(e2e.cli.cpp_audit_headers PROPERTIES LABELS "e2e;cpp;cli;audit" TIMEOUT 900)
        add_test(NAME e2e.cli.cpp_audit_macro_headers
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/smoke/cpp_audit_macro_headers.py"
                --check "$<TARGET_FILE:topo-check>"
                --extractor-dir "${_topo_audit_cpp_dir}"
                --compiler "${TOPO_AUDIT_TEST_CLANG}"
                --evidence "${CMAKE_CURRENT_BINARY_DIR}/cpp-audit-macro-headers.json")
        set_tests_properties(e2e.cli.cpp_audit_macro_headers PROPERTIES LABELS "e2e;cpp;cli;audit" TIMEOUT 900)
        add_test(NAME e2e.cli.cpp_audit_isolation
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/smoke/cpp_audit_isolation.py"
                --check "$<TARGET_FILE:topo-check>"
                --extractor-dir "${_topo_audit_cpp_dir}"
                --compiler "${TOPO_AUDIT_TEST_CLANG}")
        set_tests_properties(e2e.cli.cpp_audit_isolation PROPERTIES LABELS "e2e;cpp;cli;audit" TIMEOUT 600)
        add_test(NAME e2e.cli.cpp_audit_parent_inputs
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/smoke/cpp_audit_parent_inputs.py"
                --check "$<TARGET_FILE:topo-check>"
                --extractor-dir "${_topo_audit_cpp_dir}"
                --compiler "${TOPO_AUDIT_TEST_CLANG}"
                --evidence "${CMAKE_CURRENT_BINARY_DIR}/cpp-audit-parent-inputs.json")
        set_tests_properties(e2e.cli.cpp_audit_parent_inputs PROPERTIES LABELS "e2e;cpp;cli;audit;isolation" TIMEOUT 900)
        add_test(NAME e2e.cli.cpp_audit_path_semantics
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/smoke/cpp_audit_path_semantics.py"
                --check "$<TARGET_FILE:topo-check>"
                --extractor-dir "${_topo_audit_cpp_dir}"
                --compiler "${TOPO_AUDIT_TEST_CLANG}"
                --evidence "${CMAKE_CURRENT_BINARY_DIR}/cpp-audit-path-semantics.json")
        set_tests_properties(e2e.cli.cpp_audit_path_semantics PROPERTIES LABELS "e2e;cpp;cli;audit;isolation" TIMEOUT 900)
    endif()
endif()

# Other platforms exercise the explicit refusal path without needing an
# extractor installation. Their audit capability remains unverified.
if(TOPO_CLI_BUILD_TESTS AND NOT APPLE)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_executable(topo-analysis-launch-sentinel test/audit/AnalysisLaunchSentinel.cpp)
        foreach(language IN ITEMS cpp java)
            string(TOUPPER "${language}" language_upper)
            if(TOPO_CLI_ENABLE_${language_upper}_PLUGIN)
                add_test(NAME e2e.cli.${language}_audit_isolation_unsupported
                    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/smoke/audit_isolation_unsupported.py"
                        --check "$<TARGET_FILE:topo-check>" --language "${language}"
                        --sentinel-executable "$<TARGET_FILE:topo-analysis-launch-sentinel>")
                set_tests_properties(e2e.cli.${language}_audit_isolation_unsupported PROPERTIES
                    LABELS "e2e;${language};cli;audit" TIMEOUT 60)
            endif()
        endforeach()
    endif()
endif()

# Full Java project input path, including a real external class dependency.
# The extractor and JDK are optional in standalone C++-only installations.
set(_topo_audit_java_dir "${TOPO_AUDIT_JAVA_EXTRACTOR_DIR}")
if(TARGET topo-extract-java)
    set(_topo_audit_java_dir "${CMAKE_BINARY_DIR}")
endif()
if((TOPO_CLI_BUILD_TESTS OR TOPO_META_DRIVING_TESTS) AND _topo_audit_java_dir
        AND TOPO_GRADLE_JAVA_HOME AND TOPO_CLI_ENABLE_JAVA_PLUGIN)
    enable_testing()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME e2e.cli.java_project_model
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/smoke/java_project.py"
                --check "$<TARGET_FILE:topo-check>"
                --transpile "$<TARGET_FILE:topo-transpile>"
                --javac "${TOPO_GRADLE_JAVA_HOME}/bin/javac"
                --extractor-dir "${_topo_audit_java_dir}")
        set_tests_properties(e2e.cli.java_project_model PROPERTIES
            LABELS "e2e;java;cli;project-model" TIMEOUT 180)
        if(APPLE)
            add_test(NAME e2e.cli.java_audit
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/smoke/java_audit.py"
                    --check "$<TARGET_FILE:topo-check>" --extractor-dir "${_topo_audit_java_dir}")
            set_tests_properties(e2e.cli.java_audit PROPERTIES LABELS "e2e;java;cli;audit" TIMEOUT 900)
            add_test(NAME e2e.cli.java_audit_isolation
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/smoke/java_audit_isolation.py"
                    --check "$<TARGET_FILE:topo-check>" --extractor-dir "${_topo_audit_java_dir}"
                    --java-home "${TOPO_GRADLE_JAVA_HOME}")
            set_tests_properties(e2e.cli.java_audit_isolation PROPERTIES LABELS "e2e;java;cli;audit;isolation" TIMEOUT 600)
        endif()
        # The aggregate build adds the JVM backend after this directory.
        # Retain the command here and resolve its target after all siblings exist.
        set_property(GLOBAL PROPERTY TOPO_JAVA_BUILD_AUDIT_COMMAND
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/smoke/java_build_audit.py"
            --build "$<TARGET_FILE:topo-build>" --check "$<TARGET_FILE:topo-check>"
            --extractor-dir "${_topo_audit_java_dir}" --java-home "${TOPO_GRADLE_JAVA_HOME}")
    endif()
endif()

function(_topo_register_java_build_audit)
    get_property(_audit_command GLOBAL PROPERTY TOPO_JAVA_BUILD_AUDIT_COMMAND)
    if(APPLE AND _audit_command)
        if(TARGET topo-build-jvm-java)
            set(_backend "$<TARGET_FILE:topo-build-jvm-java>")
        elseif(TOPO_AUDIT_JAVA_BACKEND AND EXISTS "${TOPO_AUDIT_JAVA_BACKEND}")
            set(_backend "${TOPO_AUDIT_JAVA_BACKEND}")
        endif()
        if(_backend)
            add_test(NAME e2e.cli.java_build_audit COMMAND ${_audit_command} --backend "${_backend}")
            set_tests_properties(e2e.cli.java_build_audit PROPERTIES LABELS "e2e;java;cli;audit;build" TIMEOUT 600)
        endif()
    endif()
    if(TOPO_CLI_REQUIRE_AUDIT_TESTS)
        get_property(_topo_audit_source_dir GLOBAL PROPERTY TOPO_AUDIT_SOURCE_DIR)
        get_property(_registered DIRECTORY "${_topo_audit_source_dir}" PROPERTY TESTS)
        get_property(_deferred_registered DIRECTORY PROPERTY TESTS)
        list(APPEND _registered ${_deferred_registered})
        foreach(_required IN ITEMS cpp_audit cpp_audit_headers cpp_audit_macro_headers
                cpp_audit_isolation cpp_audit_parent_inputs cpp_audit_path_semantics
                java_audit java_audit_isolation java_build_audit)
            if(NOT "e2e.cli.${_required}" IN_LIST _registered)
                message(FATAL_ERROR "Required audit test e2e.cli.${_required} is missing; provide its installed extractor/backend, compiler and JDK")
            endif()
        endforeach()
    endif()
endfunction()
# A property survives deferred evaluation in an aggregate parent scope.
set_property(GLOBAL PROPERTY TOPO_AUDIT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _topo_register_java_build_audit)
