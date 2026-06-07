// topo build — End-to-end build CLI
//
// Pipeline (merge-then-optimize):
//   1. ImportResolver: resolve root .topo -> discover all modules
//   2. Semantic analysis (cross-file)
//   3-7. Delegated to backend tool via subprocess:
//        topo-build-llvm-cpp   (C++ projects)
//        topo-build-llvm-rust  (Rust projects)
//        topo-build-llvm-mixed (Mixed C++/Rust projects)

#include "Config.h"

// Plugin headers are conditionally included so a build with a subset
// of language plugins still produces a valid topo-build binary.
#if defined(TOPO_CLI_WITH_CPP_PLUGIN)
#include "CppPlugin.h"
#endif
#if defined(TOPO_CLI_WITH_RUST_PLUGIN)
#include "RustPlugin.h"
#endif
#if defined(TOPO_CLI_WITH_JAVA_PLUGIN)
#include "JavaPlugin.h"
#endif
#if defined(TOPO_CLI_WITH_PYTHON_PLUGIN)
#include "PythonPlugin.h"
#endif
#if defined(TOPO_CLI_WITH_TYPESCRIPT_PLUGIN)
#include "TypeScriptPlugin.h"
#endif

#include "topo/Basic/Diagnostic.h"
#include "topo/Build/BackendProtocol.h"
#include "topo/Build/IncrementalCache.h"
#include "topo/Debug/Emitter.h"
#include "topo/Sema/VisibilityCollector.h"
#include "topo/Lexer/Lexer.h"
#include "topo/Parser/Parser.h"
#include "topo/Platform/Platform.h"
#include "topo/Platform/Process.h"
#include "topo/Platform/SharedLibrary.h"
#include "topo/Platform/TempFile.h"
#include "topo/Analysis/LifetimeAnalysis.h"
#include "topo/Sema/ImportResolver.h"
#include "topo/Sema/SemanticAnalyzer.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <vector>

#ifdef _WIN32
#  include <process.h>
#  define TOPO_GETPID() _getpid()
#else
#  include <unistd.h>
#  define TOPO_GETPID() getpid()
#endif

namespace fs = std::filesystem;

using namespace topo::build;

/// Find the backend tool executable.
/// Search order: same directory as topo-build → PATH.
static std::string findBackendTool(const std::string& toolName) {
    // Try same directory as the running topo-build executable
    std::string exeDir = topo::platform::getExecutableDir();
    if (!exeDir.empty()) {
        fs::path candidate = fs::path(exeDir) / (toolName + std::string(topo::platform::ExeSuffix));
        if (fs::exists(candidate)) return candidate.string();
    }

    // Try PATH
    // On Unix we can use "which", on Windows "where"
    std::string whichCmd = topo::platform::IsWindows ? "where" : "which";
    auto result = topo::platform::runProcessCapture(whichCmd, {toolName + std::string(topo::platform::ExeSuffix)});
    if (result.exitCode == 0 && !result.stdoutOutput.empty()) {
        std::string path = result.stdoutOutput;
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
            path.pop_back();
        if (!path.empty() && fs::exists(path)) return path;
    }

    return "";
}

int main(int argc, char* argv[]) {
    // Register language plugins (only those linked at build time)
#if defined(TOPO_CLI_WITH_CPP_PLUGIN)
    topo::lang::registerCppPlugin();
#endif
#if defined(TOPO_CLI_WITH_RUST_PLUGIN)
    topo::lang::registerRustPlugin();
#endif
#if defined(TOPO_CLI_WITH_JAVA_PLUGIN)
    topo::lang::registerJavaPlugin();
#endif
#if defined(TOPO_CLI_WITH_PYTHON_PLUGIN)
    topo::lang::registerPythonPlugin();
#endif
#if defined(TOPO_CLI_WITH_TYPESCRIPT_PLUGIN)
    topo::lang::registerTypeScriptPlugin();
#endif

    BuildConfig cfg;

    if (argc >= 2) {
        if (!parseArgs(argc, argv, cfg)) {
            printUsage(argv[0]);
            return 1;
        }
    }

    // Try Topo.toml for any fields not set by CLI
    bool hasToml = loadTopoToml(cfg);

    // If Topo.toml exists but loadTopoToml returned false, it's a config error
    if (!hasToml && fs::exists(fs::current_path() / "Topo.toml")) {
        return 1;
    }

    // Validate: we need sources (C++ or Rust), topo root, and output
    // Mixed projects get their sources from [build.cpp], not [build].sources
    bool needsSources =
        ((cfg.language == topo::HostLanguage::Cpp || cfg.language == topo::HostLanguage::Java) && cfg.sources.empty());
    if (needsSources || cfg.topoRoot.empty() || cfg.outputPath.empty()) {
        if (!hasToml && argc < 2) {
            std::cerr << "error: no Topo.toml found and no arguments given\n\n";
        }
        if (needsSources) std::cerr << "error: no source files specified\n";
        if (cfg.topoRoot.empty()) std::cerr << "error: --topo <root.topo> is required\n";
        if (cfg.outputPath.empty()) std::cerr << "error: -o <output> is required\n";
        printUsage(argv[0]);
        return 1;
    }

    // Rust projects require Cargo.toml
    if (cfg.language == topo::HostLanguage::Rust) {
        fs::path cargoToml = fs::current_path() / "Cargo.toml";
        if (!fs::exists(cargoToml)) {
            std::cerr << "error: Rust project requires Cargo.toml\n";
            return 1;
        }
    }

    // Mixed projects require Rust manifest
    if (cfg.language == topo::HostLanguage::Mixed) {
        if (!cfg.mixedCfg.rustManifest.empty() && !fs::exists(cfg.mixedCfg.rustManifest)) {
            std::cerr << "error: Rust manifest not found: " << cfg.mixedCfg.rustManifest << "\n";
            return 1;
        }
    }

    // Auto-complete output extension based on output type + platform
    autoExtension(cfg.outputPath, cfg.outputType);

    auto t0 = std::chrono::steady_clock::now();

    // ================================================================
    // Step 0.5: Initialize incremental cache
    // ================================================================
    fs::path projectDir = fs::current_path();
    IncrementalCache cache(projectDir);
    bool useIncremental = !cfg.noIncremental;

    if (cfg.cleanCache) {
        std::cerr << "Cleaning .topo-cache/...\n";
        cache.clean();
    }

    CacheManifest manifest;
    bool cacheValid = false;
    bool topoCacheHit = false;

    if (useIncremental) {
        cache.ensureDirectories();
        cacheValid = cache.loadManifest(manifest);
    }

    // Create a temp directory for intermediate files (Steps 4-7).
    //
    // Per-PID suffix is load-bearing: this directory is `remove_all`'d at the
    // end of main() (see end of function). Without the PID, two concurrent
    // `topo-build` invocations (e.g. topo-bench-artifacts prebuild running N
    // projects in parallel) race — process A's cleanup deletes process B's
    // intermediate `optimized.ll` before B can write to it. Failure manifests
    // as `error: cannot write IR: No such file or directory` deep in
    // LLVMTransformBackend::writeIR. Regression guard for the concurrent
    // topo-build temp-directory cleanup race.
    fs::path tempDir = topo::platform::tempDirectory()
        / ("topo-build-" + std::to_string(TOPO_GETPID()));
    fs::create_directories(tempDir);

    // ================================================================
    // Step 1: ImportResolver — resolve root .topo -> all modules
    // ================================================================
    topo::DiagnosticEngine diag;
    topo::SymbolTable symbols;
    std::vector<topo::VisibilityEntry> visEntries;

    // Collect .topo file paths for cache validation
    std::vector<std::string> topoPaths;

    // Check if frontend cache is valid
    if (useIncremental && cacheValid) {
        // We need to know all .topo paths to validate the cache,
        // so we still run ImportResolver to discover modules.
        topo::ImportResolver resolver(diag);
        auto modules = resolver.resolve({cfg.topoRoot});

        if (diag.hasErrors()) {
            diag.print(std::cerr);
            return 1;
        }

        for (const auto& mod : modules)
            topoPaths.push_back(mod.path);

        if (cache.isTopoFrontendValid(manifest, topoPaths) && cache.loadSymbolTable(symbols) &&
            cache.loadVisibilityEntries(visEntries)) {
            topoCacheHit = true;
            std::cerr << "[1/7] .topo cache hit (skipping parse)\n";
            std::cerr << "[2/7] .topo cache hit (skipping sema)\n";
            std::cerr << "      " << symbols.functions().size() << " functions, " << symbols.logicBlocks().size()
                      << " logic blocks\n";
        } else {
            // Cache miss — run full frontend below
            diag = topo::DiagnosticEngine{};
            symbols = topo::SymbolTable{};
            visEntries.clear();
            topoPaths.clear();
        }
    }

    if (!topoCacheHit) {
        std::cerr << "[1/7] Resolving .topo imports...\n";

        topo::ImportResolver resolver(diag);
        auto modules = resolver.resolve({cfg.topoRoot});

        if (diag.hasErrors()) {
            diag.print(std::cerr);
            return 1;
        }

        std::cerr << "      " << modules.size() << " module(s) resolved\n";

        for (const auto& mod : modules)
            topoPaths.push_back(mod.path);

        // ================================================================
        // Step 2: Semantic analysis (per-file, respecting import boundaries)
        // ================================================================
        std::cerr << "[2/7] Running semantic analysis...\n";

        std::unordered_map<std::string, topo::SymbolTable> symbolCache;
        bool semaFailed = false;

        for (const auto& mod : modules) {
            topo::SymbolTable importedSymbols;
            for (const auto& directive : mod.imports) {
                auto it = symbolCache.find(directive.resolvedPath);
                if (it != symbolCache.end()) {
                    importedSymbols.mergeSelected(it->second, directive.selectedSymbols);
                }
            }

            topo::DiagnosticEngine fileDiag;
            topo::SemanticAnalyzer sema(fileDiag);
            auto fileSymbols = sema.analyze(static_cast<const topo::TopoFile&>(*mod.ast), importedSymbols);

            if (fileDiag.hasErrors()) {
                std::cerr << "Semantic analysis failed in '" << mod.path << "':\n";
                fileDiag.print(std::cerr);
                semaFailed = true;
                break;
            }

            symbolCache[mod.path] = fileSymbols;
            symbols = std::move(fileSymbols);
        }

        if (semaFailed) {
            return 1;
        }

        symbols = topo::SymbolTable{};
        for (const auto& mod : modules) {
            auto it = symbolCache.find(mod.path);
            if (it != symbolCache.end()) {
                symbols.mergeFrom(it->second, /*filterInternal=*/true);
            }
        }

        std::cerr << "      " << symbols.functions().size() << " functions, " << symbols.logicBlocks().size()
                  << " logic blocks\n";

        topo::VisibilityCollector collector;
        for (const auto& mod : modules) {
            auto entries = collector.collect(static_cast<const topo::TopoFile&>(*mod.ast));
            visEntries.insert(visEntries.end(), entries.begin(), entries.end());
        }

        // Save frontend results to cache
        if (useIncremental) {
            cache.saveSymbolTable(symbols);
            cache.saveVisibilityEntries(visEntries);
        }
    }

    // ================================================================
    // Update cache manifest after Steps 1-2
    // ================================================================
    if (useIncremental) {
        CacheManifest newManifest;
        // Rely on CacheManifest's default version (matches the loader's
        // required CACHE_VERSION); a hardcoded literal here drifts from the
        // loader and silently disables the frontend cache.
        newManifest.configFingerprint = IncrementalCache::computeConfigFingerprint(cfg);

        // Record .topo file mtimes
        for (const auto& path : topoPaths) {
            auto genericPath = fs::path(path).generic_string();
            newManifest.topoFileMtimes[genericPath] = IncrementalCache::getFileMtime(path);
        }

        // Record Topo.toml mtime
        fs::path tomlPath = projectDir / "Topo.toml";
        if (fs::exists(tomlPath)) {
            newManifest.topoTomlMtime = IncrementalCache::getFileMtime(tomlPath);
        }

        cache.saveManifest(newManifest);
    }

    // ================================================================
    // Step 2.5: Emit *.topo-dbg.json (base section)
    //
    // Writes <outputPath>.topo-dbg.json next to the binary so `topo debug`
    // can resolve `view <name> from <container>[start..end]` declarations
    // into the underlying container + slice. Skipped silently when the
    // project has no `debug { ... }` declarations — no debug entries means
    // no consumer, so emitting an empty symbols[] array would just be noise.
    // Backend tools later append `backend_ext.<lang>.passes[<pass_name>]`
    // segments; v1 ships only the base section so the declaration-driven
    // path (literal-bounded views over unoptimised primitives) works
    // end-to-end before any per-Pass emitter lands.
    if (!symbols.debugEntries().empty()) {
        topo::debug_meta::EmitOptions emitOpts;
        emitOpts.outPath = cfg.outputPath + ".topo-dbg.json";
        emitOpts.source.topoFiles = topoPaths;
        if (!topo::debug_meta::emit(symbols, emitOpts)) {
            std::cerr << "warning: failed to write " << emitOpts.outPath.string()
                      << " (build continues; `topo debug` view resolution will be disabled)\n";
        } else if (cfg.verbose) {
            std::cerr << "      Wrote " << emitOpts.outPath.string()
                      << " (" << symbols.debugEntries().size() << " debug entries)\n";
        }
    }

    // ================================================================
    // Step 3: Run topo-check (if --check)
    // ================================================================
    if (cfg.shouldRunCheck()) {
        std::string checkTool = findBackendTool("topo-check");
        if (checkTool.empty()) {
            std::cerr << "error: topo-check not found (required by --check)\n"
                      << "  Searched: executable directory, PATH\n"
                      << "  Ensure topo-check is built and available.\n";
            return 1;
        }

        std::cerr << "[3/7] Running topo-check...\n";
        auto checkResult = topo::platform::runProcess(checkTool,
            {"--project", projectDir.string()}, cfg.verbose);

        if (checkResult.exitCode != 0) {
            std::cerr << "error: topo-check failed (exit " << checkResult.exitCode << ")\n"
                      << "  Fix declaration issues or use --no-check to skip.\n";
            return 1;
        }
        std::cerr << "      All checks passed\n";
    }

    // ================================================================
    // Steps 4-7: Delegate to backend tool via subprocess
    // ================================================================

    // Build the BackendConfig (same as before)
    topo::backend::BackendConfig backendCfg;
    backendCfg.optLevel = cfg.optLevel;
    backendCfg.buildMode = cfg.buildMode;
    backendCfg.outputType = cfg.outputType;
    backendCfg.obfMode = cfg.obfMode;
    backendCfg.obfSalt = cfg.obfSalt;
    backendCfg.noVerify = cfg.noVerify;
    backendCfg.warnOnly = cfg.warnOnly;
    backendCfg.dumpIR = cfg.dumpIR;
    backendCfg.dumpMap = cfg.dumpMap;
    backendCfg.debugInternal = cfg.debugInternal;
    backendCfg.embedIR = cfg.embedIR;
    backendCfg.outputPath = cfg.outputPath;
    backendCfg.parallelCfg = cfg.parallelCfg;
    backendCfg.adaptiveCfg = cfg.adaptiveCfg;
    backendCfg.dataLayoutCfg = cfg.dataLayoutCfg;
    backendCfg.indirectionCfg = cfg.indirectionCfg;
    backendCfg.indirectionExplicit = cfg.indirectionExplicit;
    backendCfg.observabilityCfg = cfg.observabilityCfg;
    backendCfg.lifetimeCfg = cfg.lifetimeCfg;
    backendCfg.loopParallelCfg = cfg.loopParallelCfg;
    backendCfg.prefetchCfg = cfg.prefetchCfg;
    backendCfg.typeNarrowingCfg = cfg.typeNarrowingCfg;
    backendCfg.containmentCfg = cfg.containmentCfg;
    backendCfg.pipelineCfg = cfg.pipelineCfg;

    // Run lifetime analysis to populate scopeFunctions on each group
    if (backendCfg.lifetimeCfg.mode != topo::FeatureMode::Off) {
        auto lifetimeResult = topo::analysis::analyzeLifetimes(symbols);
        auto& groups = symbols.lifetimeGroups();
        for (size_t i = 0; i < groups.size(); ++i) {
            auto it = lifetimeResult.scopes.find(groups[i].name);
            if (it != lifetimeResult.scopes.end()) {
                std::vector<std::string> funcs(it->second.coveredFunctions.begin(),
                                                it->second.coveredFunctions.end());
                symbols.setLifetimeGroupScopeFunctions(i, std::move(funcs));
            }
        }
    }

    // Assemble the backend request
    BackendRequest req;
    req.config = backendCfg;
    req.outputPath = cfg.outputPath;
    req.symbolTable = symbols;
    req.visibilityEntries = visEntries;
    req.tempDir = tempDir.string();
    req.language = cfg.language;
    req.linkLibs = cfg.linkLibs;
    req.linkDirs = cfg.linkDirs;
    req.verbose = cfg.verbose;
    req.keepTemps = cfg.keepTemps;
    req.sources = cfg.sources;
    req.includeDirs = cfg.includeDirs;
    req.noIncremental = cfg.noIncremental;

    // Write language-specific fields into backendExtras. Each key must
    // belong to the dispatched backend's sub-schema (registered in
    // topo-core/lib/Build/BackendProtocol.cpp::knownBackendExtrasKeys).
    // The JVM backend rejects unknown keys at deserialize, so emitting
    // hostCompilerPath/standard unconditionally — as the previous code
    // did — would break every Java build.
    if (cfg.language == topo::HostLanguage::Cpp ||
        cfg.language == topo::HostLanguage::Rust ||
        cfg.language == topo::HostLanguage::Mixed) {
        req.backendExtras["hostCompilerPath"] = cfg.hostCompilerPath;
        req.backendExtras["standard"] = cfg.standard;
    }
    if (cfg.language == topo::HostLanguage::Rust) {
        req.backendExtras["cargoPath"] = cfg.cargoPath;
    }
    if (cfg.language == topo::HostLanguage::Java) {
        // Forward [build.java].target_version. Empty string means "fall back
        // to the backend's compiled-in default" — topo-build-jvm-java keeps
        // "21" as the fallback for projects that don't declare it.
        if (!cfg.javaCfg.targetVersion.empty()) {
            req.backendExtras["targetVersion"] = cfg.javaCfg.targetVersion;
        }
    }
    if (cfg.language == topo::HostLanguage::Mixed) {
        req.backendExtras["cargoPath"] = cfg.cargoPath;
        nlohmann::json mixedJ = nlohmann::json::object();
        mixedJ["cppSources"] = cfg.mixedCfg.cppSources;
        mixedJ["cppIncludeDirs"] = cfg.mixedCfg.cppIncludeDirs;
        mixedJ["cppFlags"] = cfg.mixedCfg.cppFlags;
        mixedJ["rustManifest"] = cfg.mixedCfg.rustManifest;
        req.backendExtras["mixedConfig"] = mixedJ;
    }

    // Serialize to temp JSON file
    std::string requestJson = serializeBackendRequest(req);
    fs::path requestFile = tempDir / "backend-request.json";
    {
        std::ofstream ofs(requestFile);
        if (!ofs) {
            std::cerr << "error: cannot write backend request to " << requestFile.string() << "\n";
            return 1;
        }
        ofs << requestJson;
    }

    // Find the backend tool
    std::string toolName = backendToolName(cfg.language);
    std::string toolPath = findBackendTool(toolName);
    if (toolPath.empty()) {
        std::cerr << "error: backend tool '" << toolName << "' not found\n"
                  << "  Searched: executable directory, PATH\n"
                  << "  Ensure " << toolName << " is built and available.\n";
        return 1;
    }

    // Invoke the backend tool
    std::cerr << "[4-7] Dispatching to " << toolName << "...\n";
    auto backendResult = topo::platform::runProcess(toolPath, {requestFile.string()}, cfg.verbose);

    if (backendResult.exitCode != 0) {
        std::cerr << "error: backend tool '" << toolName << "' failed (exit "
                  << backendResult.exitCode << ")\n"
                  << "  See output above for details.\n"
                  << "  To see the full command, rerun with --verbose.\n";
        return 1;
    }

    // Cleanup temp files (but NOT .topo-cache/)
    if (!cfg.keepTemps) {
        std::error_code ec;
        fs::remove_all(tempDir, ec);
    } else {
        std::cerr << "      Temps kept in " << tempDir.string() << "\n";
    }

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::cerr << "\nBuild complete: " << cfg.outputPath << " (" << ms << " ms)\n";

    return 0;
}
