# topo-init CLI executable — generates initial Topo.toml + .topo stubs
# for a project. Plugin-dispatching CLI: each Topo<Lang>Plugin provides
# the template Provider that lays down per-host scaffolding.

add_executable(topo-init
    main.cpp
    InitConfig.cpp
    TopoGenerator.cpp
)
# ALIAS for aggregated-build consumers that reference the executable
# via the install-time NAMESPACE topo::cli::.
add_executable(topo::cli::topo-init ALIAS topo-init)

# Each plugin PUBLIC-links TopoLang; TopoLang PUBLIC-propagates
# TopoCheck → TopoSema → TopoParser. Each plugin also propagates its
# host LSP bridge (TopoCppLSP / TopoRustLSP / ...) PUBLIC-depending on
# TopoLSPBridge → TopoPlatform — TopoPlatform reaches us transitively
# when at least one plugin is linked. When none are resolved we still
# need TopoLang for the registry types referenced by main.cpp.
target_link_libraries(topo-init PRIVATE
    topo::lang::TopoLang
)
if(_TOPO_CLI_PLUGIN_LINK_TARGETS)
    target_link_libraries(topo-init PRIVATE ${_TOPO_CLI_PLUGIN_LINK_TARGETS})
    target_compile_definitions(topo-init PRIVATE ${_TOPO_CLI_PLUGIN_COMPILE_DEFS})
endif()

target_include_directories(topo-init PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${TOMLPLUSPLUS_INCLUDE_DIR}
)
topo_cli_set_compiler_flags(topo-init)

# Windows/MinGW: the installed exe shared-links the MinGW runtime
# (libstdc++-6 / libgcc_s_seh-1 / libwinpthread-1), whose DLLs resolve
# from PATH at launch — stale copies (e.g. Strawberry Perl's, earlier on
# PATH) shadow the build compiler's and the tool dies silently before
# main. Static-link the runtime so the shipped tool is hermetic (same
# recipe as the find_package smoke binary).
if(MINGW)
    target_link_options(topo-init PRIVATE -static -static-libgcc -static-libstdc++)
endif()

install(TARGETS topo-init
    EXPORT topo-cliTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
