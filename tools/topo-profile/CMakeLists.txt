# topo-profile CLI executable — spans + sampling trace normalization.
#
# Plugin-free: links only topo-core's TopoPlatform / TopoProfile
# (transitively via topo-v8::Debug) and topo-v8's TopoV8Debug
# (SourceMapResolver). The JFR bridge jar baked-in macro is dropped in
# standalone mode (TOPO_GRADLE_JAVA_HOME is monorepo-only); the CLI's
# --jfr-binary-input path falls back to the runtime env-var / explicit
# CLI flag, same as in the monorepo when the gate is OFF.

add_executable(topo-profile main.cpp)

target_link_libraries(topo-profile PRIVATE
    topo::core::TopoPlatform
    # TopoProfile reaches us transitively via topo::v8::TopoV8Debug (PUBLIC).
    topo::v8::TopoV8Debug
    nlohmann_json::nlohmann_json
)
topo_cli_set_compiler_flags(topo-profile)

install(TARGETS topo-profile
    EXPORT topo-cliTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
