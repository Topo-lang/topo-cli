# TopoCliCompilerFlags.cmake — standalone compiler-flag helpers for
# topo-cli. Mirrors the monorepo cmake/TopoCompilerFlags.cmake but
# scoped to the CLI layer (zero LLVM dependency).

# RPATH configuration for Unix shared library builds.
if(NOT WIN32)
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
    if(APPLE)
        set(CMAKE_MACOSX_RPATH ON)
    endif()
endif()

# ── Sanitizer support ────────────────────────────────────
set(TOPO_CLI_SANITIZER "" CACHE STRING
    "Enable sanitizers (address, undefined, thread, memory)")

function(topo_cli_apply_sanitizer target)
    if(NOT TOPO_CLI_SANITIZER)
        return()
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target}
            PRIVATE -fsanitize=${TOPO_CLI_SANITIZER} -fno-omit-frame-pointer)
        target_link_options(${target}
            PRIVATE -fsanitize=${TOPO_CLI_SANITIZER})
    endif()
endfunction()

# Compiler flag base — applied to every C++ target in topo-cli.
function(topo_cli_set_compiler_flags target)
    target_compile_features(${target} PUBLIC cxx_std_17)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    endif()
    topo_cli_apply_sanitizer(${target})
endfunction()

# PCH helpers — no-ops in standalone (no project-wide PCH host).
function(topo_apply_std_pch target)
endfunction()
