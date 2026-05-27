# topo-transpile CLI executable — host→host source rewriting and
# .topo-source code generation. Each plugin PUBLIC-propagates its
# TopoXxxTranspile lib (which PUBLIC-depends on topo::core::TopoTranspile)
# plus TopoLang, so the transpile driver reaches main.cpp transitively.

add_executable(topo-transpile main.cpp)

target_link_libraries(topo-transpile PRIVATE
    topo::lang::TopoLang
    topo::core::TopoTranspile
)
if(_TOPO_CLI_PLUGIN_LINK_TARGETS)
    target_link_libraries(topo-transpile PRIVATE ${_TOPO_CLI_PLUGIN_LINK_TARGETS})
    target_compile_definitions(topo-transpile PRIVATE ${_TOPO_CLI_PLUGIN_COMPILE_DEFS})
endif()

target_include_directories(topo-transpile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
topo_cli_set_compiler_flags(topo-transpile)

install(TARGETS topo-transpile
    EXPORT topo-cliTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
