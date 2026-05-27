cmake_minimum_required(VERSION 3.20)

project(topo-cli
    VERSION 0.1.0
    DESCRIPTION "Topo plugin-dispatch CLI tools — topo-check / topo-build / topo-init / topo-transpile / topo-profile"
    HOMEPAGE_URL "https://github.com/topo-lang/topo-cli"
    LANGUAGES CXX
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
# topo-cli ships no unit tests in the subtree — the historical test
# fixtures lived under topo-core/test/ + each topo-lang-*'s test/
# (the deferred E2eHarness suites). The standalone build of topo-cli
# is a pure binary build; verification is via the downstream
# find_package smoke test and by running each installed CLI with
# --help / --version.
option(TOPO_CLI_BUILD_TESTS "Build topo-cli tests (no-op: no tests in this subtree)" OFF)
if(TOPO_CLI_BUILD_TESTS AND (PROJECT_IS_TOP_LEVEL OR TOPO_META_DRIVING_TESTS))
    enable_testing()
    message(STATUS "topo-cli: no in-tree tests; verification is via the find_package smoke step.")
endif()
