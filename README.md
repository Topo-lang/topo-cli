# topo-cli

The five plugin-dispatch CLI tools that drive the [Topo](https://github.com/topo-lang/topo-core)
declaration language toolchain.

| Tool | Role |
|---|---|
| `topo-check` | Verify host code obeys its `.topo` declarations. Zero LLVM dep, suitable for pre-commit. |
| `topo-build` | Build orchestrator — parses `Topo.toml`, runs frontend analysis, subprocess-dispatches the per-language backend (`topo-build-llvm-cpp`, `topo-build-jvm-java`, …). |
| `topo-init` | Generate the initial `Topo.toml` + `.topo` stubs for a project, auto-detecting the host language. |
| `topo-transpile` | Cross-language source rewriter — host→host, plus `.topo`-source codegen via the AdapterResolver. |
| `topo-profile` | Spans + sampling trace normalization, including V8 `.cpuprofile` → source-mapped frames. |

## Build

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_PREFIX_PATH=<prefix-with-topo-core+topo-lang+plugins> \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build
cmake --install build --prefix /usr/local
```

## Upstream Topo packages

| Package | Required | Notes |
|---|---|---|
| `topo-core` | yes | TopoBuildLib / TopoCheck / TopoTranspile / TopoProfile / TopoPlatform |
| `topo-lang` | yes | LanguagePlugin registry framework |
| `topo-v8` | when `topo-profile` is built | hosts `SourceMapResolver` |
| `topo-lang-cpp` | optional | `TopoCppPlugin` (gated upstream on `TOPO_LANG_CPP_ENABLE_LLVM`) |
| `topo-lang-rust` | optional | `TopoRustPlugin` (gated upstream on `TOPO_LANG_RUST_ENABLE_LLVM`) |
| `topo-lang-java` | optional | `TopoJavaPlugin` |
| `topo-lang-python` | optional | `TopoPythonPlugin` |
| `topo-lang-typescript` | optional | `TopoTypeScriptPlugin` (needs `topo-v8`) |

Each plugin is detected at configure time. Missing or gated-out plugins
are auto-disabled with a `STATUS` message and dropped from the
corresponding CLI's plugin-registration block via the
`TOPO_CLI_WITH_<LANG>_PLUGIN` compile definitions.

## Backend tools at runtime

`topo-build` dispatches to per-language backend tools by subprocess
spawn. The backend binaries live in their respective upstream packages:

| Backend tool | Installed by |
|---|---|
| `topo-build-llvm-cpp` / `topo-build-llvm-rust` / `topo-build-llvm-mixed` | `topo-llvm` (when `TOPO_LLVM_BUILD_TOOLS=ON`) |
| `topo-build-jvm-java` | `topo-jvm` |
| `topo-build-python` | `topo-lang-python` |
| `topo-build-typescript` | `topo-lang-typescript` |

Install all of `topo-cli` plus the relevant backend packages into the
same prefix, or add each package's `bin/` to `PATH`, so `topo-build`'s
runtime resolver can find them.

## Consuming `topo-cli`

```cmake
find_package(topo-cli CONFIG REQUIRED)
# Link the check-runner library directly into an e2e harness:
target_link_libraries(my-harness PRIVATE topo::cli::TopoCheckRunner)
# Or spawn the installed CLIs as subprocesses (their paths are also
# importable as topo::cli::topo-check / topo::cli::topo-build / ...).
```

## License

See [LICENSE](LICENSE).
