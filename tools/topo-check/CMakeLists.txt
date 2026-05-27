# topo-check — TopoCheckRunner library + topo-check CLI executable.
#
# TopoCheckRunner is exported (topo::cli::TopoCheckRunner) so downstream
# checker-e2e suites in topo-lang-* packages can link it without
# reaching into topo-cli's tools/ source tree. The CLI links it plus
# every resolved Topo<Lang>Plugin aggregator and registers each one at
# main() entry through TOPO_CLI_WITH_<LANG>_PLUGIN compile defs.

add_library(TopoCheckRunner STATIC CheckRunner.cpp)
add_library(topo::cli::TopoCheckRunner ALIAS TopoCheckRunner)

# topo::lang::TopoLang PUBLIC-propagates topo::core::TopoCheck →
# TopoSema → TopoParser; TopoCheck brings TopoAnalysis. TopoPlatform
# stays explicit (no transitive path).
target_link_libraries(TopoCheckRunner PUBLIC
    topo::lang::TopoLang
    topo::core::TopoPlatform
    nlohmann_json::nlohmann_json
)
target_include_directories(TopoCheckRunner
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/topo-cli/check>
    PRIVATE
        ${TOMLPLUSPLUS_INCLUDE_DIR}
)
topo_cli_set_compiler_flags(TopoCheckRunner)

install(TARGETS TopoCheckRunner
    EXPORT topo-cliTargets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-cli/check
)
install(FILES CheckRunner.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-cli/check
)

# ── topo-check CLI ─────────────────────────────────────
add_executable(topo-check main.cpp)
target_link_libraries(topo-check PRIVATE TopoCheckRunner)
if(_TOPO_CLI_PLUGIN_LINK_TARGETS)
    target_link_libraries(topo-check PRIVATE ${_TOPO_CLI_PLUGIN_LINK_TARGETS})
    target_compile_definitions(topo-check PRIVATE ${_TOPO_CLI_PLUGIN_COMPILE_DEFS})
endif()
topo_cli_set_compiler_flags(topo-check)

install(TARGETS topo-check
    EXPORT topo-cliTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
