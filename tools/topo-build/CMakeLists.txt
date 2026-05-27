# topo-build CLI executable (ZERO LLVM dependency).
#
# This is the thin router that parses Topo.toml, runs frontend
# analysis, serializes a BackendRequest JSON, and subprocess-dispatches
# the language-specific backend tool (topo-build-llvm-cpp /
# topo-build-llvm-rust / topo-build-jvm-java / topo-build-python /
# topo-build-typescript). The backend tools themselves are installed
# by their respective upstream packages (topo-llvm / topo-jvm /
# topo-lang-python / topo-lang-typescript). At runtime topo-build
# locates them via PATH (or the same directory as the topo-build
# binary).
#
# In monorepo mode this CMakeLists symlinked the backend-tool binaries
# next to topo-build at build time so topo-build's PATH search hit
# them without install. In standalone mode the backend tools live in
# their upstream packages' install set; consumers add the upstream
# install bin/ to PATH (or install both into the same prefix).

add_executable(topo-build
    main.cpp
    Config.cpp
)
# ALIAS so an aggregated parent build can reference this executable via
# its namespaced name (matches the install-time NAMESPACE topo::cli::
# under which downstream packages like topo-llvm/tools-late resolve it).
add_executable(topo::cli::topo-build ALIAS topo-build)

# topo-core's TopoBuildLib PUBLIC-propagates TopoPlatform / TopoSema /
# TopoLang transitively. Each plugin aggregator PUBLIC-propagates
# TopoLang → TopoCheck → TopoAnalysis / TopoSema / TopoParser plus its
# per-language driver lib.
target_link_libraries(topo-build PRIVATE
    topo::core::TopoBuildConfig
    topo::core::TopoBuildLib
    topo::core::TopoDebug
)
if(_TOPO_CLI_PLUGIN_LINK_TARGETS)
    target_link_libraries(topo-build PRIVATE ${_TOPO_CLI_PLUGIN_LINK_TARGETS})
    target_compile_definitions(topo-build PRIVATE ${_TOPO_CLI_PLUGIN_COMPILE_DEFS})
endif()

target_include_directories(topo-build PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${TOMLPLUSPLUS_INCLUDE_DIR}
)

topo_cli_set_compiler_flags(topo-build)

install(TARGETS topo-build
    EXPORT topo-cliTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
