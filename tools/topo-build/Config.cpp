#include "Config.h"

#include "topo/Build/ConfigValidator.h"
#include "topo/Platform/Platform.h"
#include "topo/Platform/FileGlob.h"
#include "topo/Platform/SharedLibrary.h"  // platform::getExecutableDir()

// toml++ as header-only, no-exception mode (LLVM is built with -fno-rtti)
#define TOML_HEADER_ONLY 1
#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace topo::build {

/// Warn about unrecognized keys in a TOML section.
/// Returns the number of unknown keys found.
static int warnUnknownKeys(const toml::table& section,
                           const std::string& sectionName,
                           const std::set<std::string>& knownKeys) {
    int count = 0;
    for (const auto& [key, val] : section) {
        if (knownKeys.find(std::string(key)) == knownKeys.end()) {
            std::cerr << "warning: unknown key '" << key << "' in " << sectionName << " (ignored)\n";
            ++count;
        }
    }
    return count;
}

static bool resolveAndApplyProfile(build::BuildConfig& cfg) {
    if (cfg.selectedProfile.empty()) return true;

    // Merge built-in profiles as defaults (user profiles override)
    auto allProfiles = builtinProfiles();
    for (const auto& [name, overrides] : cfg.profiles) {
        allProfiles[name] = overrides; // user wins
    }

    auto it = allProfiles.find(cfg.selectedProfile);
    if (it == allProfiles.end()) {
        std::cerr << "error: unknown profile '" << cfg.selectedProfile << "'\n";
        std::cerr << "  available profiles:";
        for (const auto& [name, unused] : allProfiles)
            std::cerr << " " << name;
        std::cerr << "\n";
        return false;
    }

    // Resolve extends chain, collect overrides from base to derived
    std::vector<const ProfileOverrides*> chain;
    std::set<std::string> visited;
    std::string current = cfg.selectedProfile;
    while (!current.empty()) {
        if (visited.count(current)) {
            std::cerr << "error: circular profile extends: '" << current << "'\n";
            return false;
        }
        visited.insert(current);
        auto cit = allProfiles.find(current);
        if (cit == allProfiles.end()) {
            std::cerr << "error: profile '" << current << "' extends unknown profile\n";
            return false;
        }
        chain.push_back(&cit->second);
        current = cit->second.extends.value_or("");
    }

    // Apply from base (last) to derived (first) — derived overrides base
    auto apply = [&](const ProfileOverrides& p) {
        if (p.buildMode) cfg.buildMode = *p.buildMode;
        if (p.optLevel) cfg.optLevel = *p.optLevel;
        if (p.obfMode) cfg.obfMode = *p.obfMode;
        if (p.obfSalt) cfg.obfSalt = *p.obfSalt;
        if (p.embedIR) cfg.embedIR = *p.embedIR;
        if (p.parallel) cfg.parallelCfg.mode = *p.parallel;
        if (p.adaptive) cfg.adaptiveCfg.mode = *p.adaptive;
        if (p.dataLayout) cfg.dataLayoutCfg.mode = *p.dataLayout;
        if (p.indirection) cfg.indirectionCfg.mode = *p.indirection;
        if (p.observability) cfg.observabilityCfg.mode = *p.observability;
        if (p.observabilityExporter) cfg.observabilityCfg.exporter = *p.observabilityExporter;
        if (p.lifetime) cfg.lifetimeCfg.mode = *p.lifetime;
        if (p.loopParallel) cfg.loopParallelCfg.mode = *p.loopParallel;
        if (p.prefetch) cfg.prefetchCfg.mode = *p.prefetch;
        if (p.typeNarrowing) cfg.typeNarrowingCfg.mode = *p.typeNarrowing;
        if (p.containment) cfg.containmentCfg.mode = *p.containment;
        if (p.parallelInstrument) cfg.parallelCfg.instrument = *p.parallelInstrument;
    };

    // Apply base-to-derived (reverse order)
    for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
        apply(**rit);
    }

    return true;
}

void printUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [<cpp-sources...> --topo <root.topo> -o <output>] [options]\n"
              << "\nWith no arguments, reads Topo.toml from the current directory.\n"
              << "\nOptions:\n"
              << "  -o <output>           Output binary\n"
              << "  --topo <root.topo>    Root .topo file\n"
              << "  -I <dir>              C++ include path (repeatable)\n"
              << "  -std=<standard>       C++ standard (default: c++17)\n"
              << "  -O<level>             Optimization level 0-3 (default: 2)\n"
              << "  --no-verify           Skip Topo/IR verification\n"
              << "  --warn-only           Report verification failures as warnings (migration aid)\n"
              << "  --dump-ir             Output optimized IR to <output>.ll\n"
              << "  --dump-map            Print symbol mapping\n"
              << "  --keep-temps          Keep intermediate files\n"
              << "  --verbose             Print executed commands\n"
              << "  --debug-internal      Preserve internal symbols with __topo_internal_ prefix\n"
              << "  --mode <mode>         Build mode: dev (default) or aggressive\n"
              << "  --output-type <type>  Output type: exe (default), shared, or static\n"
              << "  --host-compiler <path> Host compiler path (forwarded to backend)\n"
              << "  --profile <name>      Apply a build profile (embedded, server, wasm, desktop, or custom)\n"
              << "  --no-incremental      Force full rebuild (skip cache)\n"
              << "  --clean               Remove .topo-cache/ before building\n"
              << "  --check               Run topo-check before building (overrides [build].check)\n"
              << "  --no-check            Skip topo-check (overrides [build].check)\n"
              << "\nExamples:\n"
              << "  topo-build                                    Build using Topo.toml\n"
              << "  topo-build src/*.cpp --topo main.topo -o app  Build from CLI args\n"
              << "  topo-build --verbose --dump-ir                Build with diagnostics\n";
}

bool parseArgs(int argc, char* argv[], BuildConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--topo" && i + 1 < argc) {
            cfg.topoRoot = argv[++i];
        } else if (arg == "-o" && i + 1 < argc) {
            cfg.outputPath = argv[++i];
        } else if (arg == "-I" && i + 1 < argc) {
            cfg.includeDirs.push_back(argv[++i]);
        } else if (arg.size() > 2 && arg.substr(0, 2) == "-I") {
            cfg.includeDirs.push_back(arg.substr(2));
        } else if (arg.substr(0, 5) == "-std=") {
            cfg.standard = arg.substr(5);
        } else if (arg.size() == 3 && arg[0] == '-' && arg[1] == 'O' && arg[2] >= '0' && arg[2] <= '3') {
            cfg.optLevel = static_cast<OptLevel>(arg[2] - '0');
        } else if (arg == "--no-verify") {
            cfg.noVerify = true;
        } else if (arg == "--warn-only") {
            cfg.warnOnly = true;
        } else if (arg == "--dump-ir") {
            cfg.dumpIR = true;
        } else if (arg == "--dump-map") {
            cfg.dumpMap = true;
        } else if (arg == "--keep-temps") {
            cfg.keepTemps = true;
        } else if (arg == "--verbose") {
            cfg.verbose = true;
        } else if (arg == "--debug-internal") {
            cfg.debugInternal = true;
        } else if (arg == "--obfuscation" && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "salted") {
                cfg.obfMode = ObfuscationMode::Salted;
            } else if (mode == "normal") {
                cfg.obfMode = ObfuscationMode::Normal;
            } else {
                std::cerr << "error: unknown obfuscation mode '" << mode << "'\n";
                return false;
            }
        } else if (arg == "--mode" && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "aggressive") {
                cfg.buildMode = BuildMode::Aggressive;
            } else if (mode == "dev") {
                cfg.buildMode = BuildMode::Dev;
            } else {
                std::cerr << "error: unknown build mode '" << mode << "' (use 'dev' or 'aggressive')\n";
                return false;
            }
        } else if (arg == "--output-type" && i + 1 < argc) {
            std::string type = argv[++i];
            if (type == "exe") {
                cfg.outputType = OutputType::Exe;
            } else if (type == "shared") {
                cfg.outputType = OutputType::Shared;
            } else if (type == "static") {
                cfg.outputType = OutputType::Static;
            } else {
                std::cerr << "error: unknown output type '" << type << "' (use 'exe', 'shared', or 'static')\n";
                return false;
            }
        } else if (arg == "--host-compiler" && i + 1 < argc) {
            cfg.hostCompilerPath = argv[++i];
        } else if (arg == "--profile" && i + 1 < argc) {
            cfg.selectedProfile = argv[++i];
        } else if (arg == "--no-incremental") {
            cfg.noIncremental = true;
        } else if (arg == "--clean") {
            cfg.cleanCache = true;
        } else if (arg == "--check") {
            cfg.checkCliOverride = true;
        } else if (arg == "--no-check") {
            cfg.checkCliOverride = false;
        } else if (arg[0] != '-') {
            // Positional: must be a .cpp/.cc/.cxx source file
            if (platform::endsWith(arg, ".cpp") || platform::endsWith(arg, ".cc") || platform::endsWith(arg, ".cxx") ||
                platform::endsWith(arg, ".c")) {
                cfg.sources.push_back(arg);
            } else {
                std::cerr << "error: unrecognized file '" << arg << "'\n";
                return false;
            }
        } else {
            std::cerr << "error: unknown option '" << arg << "'\n";
            return false;
        }
    }

    return true;
}

bool loadTopoToml(BuildConfig& cfg) {
    fs::path tomlPath = fs::current_path() / "Topo.toml";
    if (!fs::exists(tomlPath)) return false;

    std::cerr << "Reading " << tomlPath.string() << "\n";

    toml::parse_result result = toml::parse_file(tomlPath.string());
    if (!result) {
        std::cerr << "error: failed to parse Topo.toml: " << result.error() << "\n";
        return false;
    }
    toml::table& tbl = result.table();

    fs::path baseDir = tomlPath.parent_path();

    // --- Unknown key detection for top-level sections ---
    if (auto* topoSec = tbl["topo"].as_table()) {
        warnUnknownKeys(*topoSec, "[topo]", {"root"});
    }
    if (auto* projectSec = tbl["project"].as_table()) {
        warnUnknownKeys(*projectSec, "[project]", {"name"});
    }
    if (auto* buildSec = tbl["build"].as_table()) {
        warnUnknownKeys(*buildSec,
                        "[build]",
                        {"language",
                         "sources",
                         "include",
                         "standard",
                         "output",
                         "output_type",
                         "embed_ir",
                         "incremental",
                         "check",
                         "link_libs",
                         "link_dirs",
                         "cpp",
                         "rust"});
    }
    if (auto* builderSec = tbl["builder"].as_table()) {
        warnUnknownKeys(
            *builderSec, "[builder]", {"mode", "obfuscation", "obfuscation_salt", "debug_internal", "warn_only"});
    }

    // Warn about unknown top-level sections
    {
        static const std::set<std::string> knownSections = {"topo",
                                                            "project",
                                                            "build",
                                                            "builder",
                                                            "parallel",
                                                            "adaptive",
                                                            "optimize",
                                                            "observability",
                                                            "lifetime",
                                                            "loop_parallel",
                                                            "containment",
                                                            "completeness",
                                                            "test",
                                                            "purity",
                                                            "visibility",
                                                            "stage_isolation",
                                                            "transforms",
                                                            "types",
                                                            "pipeline",
                                                            "profile"};
        for (const auto& [key, val] : tbl) {
            if (knownSections.find(std::string(key)) == knownSections.end()) {
                std::cerr << "warning: unknown section [" << key << "] (ignored)\n";
            }
        }
    }

    // [topo].root (required)
    if (auto root = tbl["topo"]["root"].value<std::string>()) {
        if (cfg.topoRoot.empty()) {
            cfg.topoRoot = (baseDir / *root).string();
        }
    }

    // [build].language — "cpp" (default), "rust", "java", "python", or "mixed"
    if (auto lang = tbl["build"]["language"].value<std::string>()) {
        cfg.language = parseHostLanguage(*lang);
    }

    // [build.java] — Java-specific knobs (parsed for any language so an
    // accidental misdeclaration in a non-Java project still surfaces, but
    // only consumed when language == Java).
    if (auto* javaTbl = tbl["build"]["java"].as_table()) {
        warnUnknownKeys(*javaTbl, "[build.java]", {"target_version"});
        if (auto v = (*javaTbl)["target_version"].value<std::string>()) {
            cfg.javaCfg.targetVersion = *v;
        }
    }

    // [build.cpp] and [build.rust] — mixed C++/Rust project subsections
    if (cfg.language == HostLanguage::Mixed) {
        if (auto* cppTbl = tbl["build"]["cpp"].as_table()) {
            if (auto* srcs = (*cppTbl)["sources"].as_array()) {
                for (const auto& elem : *srcs) {
                    if (auto pat = elem.value<std::string>()) {
                        auto expanded = platform::globExpand(baseDir, *pat);
                        cfg.mixedCfg.cppSources.insert(cfg.mixedCfg.cppSources.end(), expanded.begin(), expanded.end());
                    }
                }
            }
            if (auto* incs = (*cppTbl)["include"].as_array()) {
                for (const auto& elem : *incs) {
                    if (auto dir = elem.value<std::string>()) {
                        cfg.mixedCfg.cppIncludeDirs.push_back((baseDir / *dir).string());
                    }
                }
            }
            if (auto* flags = (*cppTbl)["flags"].as_array()) {
                for (const auto& elem : *flags) {
                    if (auto sv = elem.value<std::string>()) cfg.mixedCfg.cppFlags.push_back(*sv);
                }
            }
        }
        if (auto* rustTbl = tbl["build"]["rust"].as_table()) {
            if (auto m = (*rustTbl)["manifest"].value<std::string>()) {
                cfg.mixedCfg.rustManifest = (baseDir / *m).string();
            }
        }
    }

    // [build].sources — array of glob patterns
    if (cfg.sources.empty()) {
        if (auto* sources = tbl["build"]["sources"].as_array()) {
            for (const auto& elem : *sources) {
                if (auto pat = elem.value<std::string>()) {
                    auto expanded = platform::globExpand(baseDir, *pat);
                    cfg.sources.insert(cfg.sources.end(), expanded.begin(), expanded.end());
                }
            }
        }
    }

    // [build].include — array of include directories
    if (cfg.includeDirs.empty()) {
        if (auto* includes = tbl["build"]["include"].as_array()) {
            for (const auto& elem : *includes) {
                if (auto dir = elem.value<std::string>()) {
                    cfg.includeDirs.push_back((baseDir / *dir).string());
                }
            }
        }
    }

    // Binary-relative stdlib include fallback. The project's `include` paths
    // above are resolved relative to Topo.toml; in a standalone build outside
    // the meta sibling layout (e.g. a benchmark whose include list points at
    // `../../../topo-lang-cpp/runtime/include`), the topo-lang-cpp stdlib
    // headers (`<topo/...>`) those paths target may not exist on disk. The
    // toolchain's own installed headers live next to the binary at
    // <prefix>/include (topo-build installs to <prefix>/bin/topo-build), so add
    // that directory as a fallback search path — letting `#include <topo/...>`
    // resolve without a manual -I or a build-side header symlink. Added only
    // when it exists, so an uninstalled build-tree layout is unaffected.
    {
        fs::path exeDir = platform::getExecutableDir();
        if (!exeDir.empty()) {
            fs::path prefixInclude = exeDir.parent_path() / "include";
            std::error_code ec;
            if (fs::is_directory(prefixInclude, ec)) {
                std::string p = prefixInclude.string();
                if (std::find(cfg.includeDirs.begin(), cfg.includeDirs.end(), p)
                    == cfg.includeDirs.end()) {
                    cfg.includeDirs.push_back(p);
                }
            }
        }
    }

    // [build].standard
    if (auto std = tbl["build"]["standard"].value<std::string>()) {
        if (cfg.standard == "c++17") { // only override if still default
            cfg.standard = *std;
        }
    }

    // [build].output
    if (auto out = tbl["build"]["output"].value<std::string>()) {
        if (cfg.outputPath.empty()) {
            cfg.outputPath = (baseDir / *out).string();
        }
    }

    // [build].output_type
    if (auto otype = tbl["build"]["output_type"].value<std::string>()) {
        if (*otype == "exe") {
            cfg.outputType = OutputType::Exe;
        } else if (*otype == "shared") {
            cfg.outputType = OutputType::Shared;
        } else if (*otype == "static") {
            cfg.outputType = OutputType::Static;
        }
        // Unknown output_type values are caught by validateConfig below
    }

    // [builder].obfuscation
    if (auto obf = tbl["builder"]["obfuscation"].value<std::string>()) {
        cfg.rawObfuscation = *obf;
        if (*obf == "salted") {
            cfg.obfMode = ObfuscationMode::Salted;
        } else if (*obf == "normal") {
            cfg.obfMode = ObfuscationMode::Normal;
        }
        // Unknown values are caught by validateConfig below
    }

    // [builder].obfuscation_salt
    if (auto salt = tbl["builder"]["obfuscation_salt"].value<std::string>()) {
        cfg.obfSalt = *salt;
    }

    // Auto-generate salt if salted obfuscation but no salt provided
    if (cfg.obfMode == ObfuscationMode::Salted && cfg.obfSalt.empty()) {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::ostringstream oss;
        oss << std::hex << std::setfill('0') << std::setw(16) << gen();
        cfg.obfSalt = oss.str();
    }

    // [builder].debug_internal
    if (auto dbgInt = tbl["builder"]["debug_internal"].value<bool>()) {
        cfg.debugInternal = *dbgInt;
    }

    // [builder].warn_only
    if (auto warnOnly = tbl["builder"]["warn_only"].value<bool>()) {
        cfg.warnOnly = *warnOnly;
    }

    // [builder].mode
    if (auto mode = tbl["builder"]["mode"].value<std::string>()) {
        if (*mode == "aggressive") {
            cfg.buildMode = BuildMode::Aggressive;
        }
        // Unknown mode values are caught by validateConfig below
    }

    // [build].embed_ir
    if (auto embed = tbl["build"]["embed_ir"].value<bool>()) {
        cfg.embedIR = *embed;
    }

    // [build].incremental — opt-out of incremental builds (default: true)
    if (auto incr = tbl["build"]["incremental"].value<bool>()) {
        if (!*incr) cfg.noIncremental = true;
    }

    // [build].check — on / off / auto (default auto). Invalid values are caught by validateConfig below.
    if (auto chk = tbl["build"]["check"].value<std::string>()) {
        cfg.rawCheck = *chk;
        bool ok = false;
        auto mode = parseCheckMode(*chk, ok);
        if (ok) cfg.checkMode = mode;
    }

    // [build].link_libs — array of library names
    if (auto* libs = tbl["build"]["link_libs"].as_array()) {
        for (const auto& elem : *libs) {
            if (auto lib = elem.value<std::string>()) cfg.linkLibs.push_back(*lib);
        }
    }

    // [build].link_dirs — array of library search directories
    if (auto* dirs = tbl["build"]["link_dirs"].as_array()) {
        for (const auto& elem : *dirs) {
            if (auto dir = elem.value<std::string>()) cfg.linkDirs.push_back((baseDir / *dir).string());
        }
    }

    // Helper: parse mode field with backward-compat for "enabled" bool.
    // mode = "off"/"auto"/"force" takes priority; "enabled" is deprecated fallback.
    auto parseMode = [](const toml::table& section, const std::string& sectionName) -> FeatureMode {
        if (auto mode = section["mode"].value<std::string>()) return parseFeatureMode(*mode);
        // Backward compat: enabled = true → Auto, false → Off
        if (auto enabled = section["enabled"].value<bool>()) {
            std::cerr << "warning: " << sectionName
                      << ".enabled is deprecated, use mode = \"off\"/\"auto\"/\"force\"\n";
            return *enabled ? FeatureMode::Auto : FeatureMode::Off;
        }
        return FeatureMode::Off;
    };

    // [parallel] section
    if (auto* par = tbl["parallel"].as_table()) {
        // Note: `min_tasks_to_parallelize` was removed — Topo passes
        // do not gate on workload-side cost heuristics. Key omitted from
        // known list; presence in Topo.toml emits a deprecation warning via
        // warnUnknownKeys.
        warnUnknownKeys(*par,
                        "[parallel]",
                        {"mode",
                         "enabled",
                         "instrument",
                         "exclude",
                         "benchmark_iterations",
                         "benchmark_warmup"});
        cfg.parallelCfg.mode = parseMode(*par, "[parallel]");
        if (auto inst = (*par)["instrument"].value<bool>()) cfg.parallelCfg.instrument = *inst;
        if (auto iters = (*par)["benchmark_iterations"].value<int64_t>())
            cfg.parallelCfg.benchmarkIterations = static_cast<int>(*iters);
        if (auto warmup = (*par)["benchmark_warmup"].value<int64_t>())
            cfg.parallelCfg.benchmarkWarmup = static_cast<int>(*warmup);
        if (auto* excl = (*par)["exclude"].as_array()) {
            for (const auto& elem : *excl) {
                if (auto name = elem.value<std::string>()) cfg.parallelCfg.exclude.push_back(*name);
            }
        }
    }

    // [pipeline] section — gates PipelineCodeGenPass (LLVM) / PipelinePass (JVM).
    // Default (no section OR no mode key) leaves mode = Auto: DAG rewrite runs
    // whenever a pipeline logic block exists.  `mode = "off"` disables the
    // rewrite entirely (used by benchmark base configs to compare against the
    // unwritten baseline).  Unlike most feature sections PipelineConfig
    // defaults to Auto rather than Off, so we only override when the user
    // supplies an explicit mode value.
    if (auto* pipe = tbl["pipeline"].as_table()) {
        warnUnknownKeys(*pipe, "[pipeline]", {"mode"});
        if (auto mode = (*pipe)["mode"].value<std::string>()) {
            cfg.pipelineCfg.mode = parseFeatureMode(*mode);
        }
    }

    // Check [optimize] sub-sections
    if (auto* optSec = tbl["optimize"].as_table()) {
        warnUnknownKeys(*optSec, "[optimize]", {"data-layout", "indirection", "prefetch", "type-narrowing"});
    }

    // [optimize.data-layout] section
    if (auto* dl = tbl["optimize"]["data-layout"].as_table()) {
        // Note: `min_array_size` was removed — Topo passes do not gate
        // on workload-side cost heuristics. Key omitted from known list;
        // presence in Topo.toml emits a deprecation warning.
        warnUnknownKeys(
            *dl,
            "[optimize.data-layout]",
            {"mode", "enabled", "auto_select", "benchmark_iterations", "benchmark_warmup"});
        if (auto mode = (*dl)["mode"].value<std::string>()) {
            // "soa"/"aos" are accepted aliases for back-compat; everything else
            // delegates to the generic parseFeatureMode so "force" works too.
            if (*mode == "soa") {
                cfg.dataLayoutCfg.mode = FeatureMode::Force;
            } else if (*mode == "aos") {
                cfg.dataLayoutCfg.mode = FeatureMode::Off;
            } else {
                cfg.dataLayoutCfg.mode = parseFeatureMode(*mode);
            }
        }
        // Backward compat for old fields
        if (auto enabled = (*dl)["enabled"].value<bool>()) {
            std::cerr << "warning: [optimize.data-layout].enabled is deprecated, use mode\n";
            if (!*enabled)
                cfg.dataLayoutCfg.mode = FeatureMode::Off;
            else {
                if (auto autoSel = (*dl)["auto_select"].value<bool>(); autoSel && *autoSel)
                    cfg.dataLayoutCfg.mode = FeatureMode::Auto;
                else
                    cfg.dataLayoutCfg.mode = FeatureMode::Force;
            }
        }
        if (auto benchIter = (*dl)["benchmark_iterations"].value<int64_t>())
            cfg.dataLayoutCfg.benchmarkIterations = static_cast<int>(*benchIter);
        if (auto benchWarm = (*dl)["benchmark_warmup"].value<int64_t>())
            cfg.dataLayoutCfg.benchmarkWarmup = static_cast<int>(*benchWarm);
    }

    // [optimize.indirection] section
    if (auto* ind = tbl["optimize"]["indirection"].as_table()) {
        warnUnknownKeys(*ind,
                        "[optimize.indirection]",
                        {"mode",
                         "enabled",
                         "unique_ptr_promotion",
                         "shared_ptr_exclusive",
                         "vector_span_lowering",
                         "pointer_attr_inference",
                         "devirtualize",
                         "benchmark_iterations",
                         "benchmark_warmup"});
        cfg.indirectionExplicit = true;
        cfg.indirectionCfg.mode = parseMode(*ind, "[optimize.indirection]");
        if (auto uptr = (*ind)["unique_ptr_promotion"].value<bool>()) cfg.indirectionCfg.uniquePtrPromotion = *uptr;
        if (auto sptr = (*ind)["shared_ptr_exclusive"].value<bool>()) cfg.indirectionCfg.sharedPtrExclusive = *sptr;
        if (auto vspan = (*ind)["vector_span_lowering"].value<bool>()) cfg.indirectionCfg.vectorSpanLowering = *vspan;
        if (auto pattr = (*ind)["pointer_attr_inference"].value<bool>())
            cfg.indirectionCfg.pointerAttrInference = *pattr;
        if (auto devirt = (*ind)["devirtualize"].value<bool>()) cfg.indirectionCfg.devirtualize = *devirt;
        if (auto benchIter = (*ind)["benchmark_iterations"].value<int64_t>())
            cfg.indirectionCfg.benchmarkIterations = static_cast<int>(*benchIter);
        if (auto benchWarm = (*ind)["benchmark_warmup"].value<int64_t>())
            cfg.indirectionCfg.benchmarkWarmup = static_cast<int>(*benchWarm);
    }

    // [optimize.prefetch] section
    if (auto* pf = tbl["optimize"]["prefetch"].as_table()) {
        warnUnknownKeys(*pf, "[optimize.prefetch]", {"mode", "distance"});
        cfg.prefetchCfg.mode = parseMode(*pf, "[optimize.prefetch]");
        if (auto dist = (*pf)["distance"].value<int64_t>())
            cfg.prefetchCfg.distance = static_cast<int>(*dist);
    }

    // [optimize.type-narrowing] section
    if (auto* tn = tbl["optimize"]["type-narrowing"].as_table()) {
        warnUnknownKeys(*tn, "[optimize.type-narrowing]", {"mode"});
        cfg.typeNarrowingCfg.mode = parseMode(*tn, "[optimize.type-narrowing]");
    }

    // [adaptive] section
    if (auto* adp = tbl["adaptive"].as_table()) {
        warnUnknownKeys(
            *adp, "[adaptive]", {"mode", "enabled", "min_trigger_ns", "benchmark_iterations", "benchmark_warmup"});
        cfg.adaptiveCfg.mode = parseMode(*adp, "[adaptive]");
        if (auto minTrigger = (*adp)["min_trigger_ns"].value<int64_t>())
            cfg.adaptiveCfg.min_trigger_ns = static_cast<uint64_t>(*minTrigger);
        if (auto benchIter = (*adp)["benchmark_iterations"].value<int64_t>())
            cfg.adaptiveCfg.benchmarkIterations = static_cast<int>(*benchIter);
        if (auto benchWarm = (*adp)["benchmark_warmup"].value<int64_t>())
            cfg.adaptiveCfg.benchmarkWarmup = static_cast<int>(*benchWarm);
    }

    // [observability] section
    if (auto* obs = tbl["observability"].as_table()) {
        warnUnknownKeys(*obs, "[observability]", {"mode", "enabled", "exporter", "sampling_rate", "internal_stages"});
        cfg.observabilityCfg.mode = parseMode(*obs, "[observability]");
        if (auto exporter = (*obs)["exporter"].value<std::string>()) cfg.observabilityCfg.exporter = *exporter;
        if (auto rate = (*obs)["sampling_rate"].value<double>()) cfg.observabilityCfg.samplingRate = *rate;
        if (auto internal = (*obs)["internal_stages"].value<bool>()) cfg.observabilityCfg.internalStages = *internal;
    }

    // [lifetime] section
    if (auto* lt = tbl["lifetime"].as_table()) {
        warnUnknownKeys(
            *lt, "[lifetime]", {"mode", "enabled", "default_arena_size", "benchmark_iterations", "benchmark_warmup"});
        cfg.lifetimeCfg.mode = parseMode(*lt, "[lifetime]");
        if (auto arenaSize = (*lt)["default_arena_size"].value<int64_t>())
            cfg.lifetimeCfg.defaultArenaSize = static_cast<size_t>(*arenaSize);
        if (auto benchIter = (*lt)["benchmark_iterations"].value<int64_t>())
            cfg.lifetimeCfg.benchmarkIterations = static_cast<int>(*benchIter);
        if (auto benchWarm = (*lt)["benchmark_warmup"].value<int64_t>())
            cfg.lifetimeCfg.benchmarkWarmup = static_cast<int>(*benchWarm);
    }

    // [loop_parallel] section
    if (auto* lp = tbl["loop_parallel"].as_table()) {
        warnUnknownKeys(*lp,
                        "[loop_parallel]",
                        {"mode",
                         "enabled",
                         "exclude",
                         "benchmark_iterations",
                         "benchmark_warmup",
                         "partition",
                         "partition_strategy",
                         "reduction",
                         "chunk_size",
                         "instrument"});
        cfg.loopParallelCfg.mode = parseMode(*lp, "[loop_parallel]");
        if (auto* excl = (*lp)["exclude"].as_array()) {
            for (const auto& elem : *excl) {
                if (auto name = elem.value<std::string>()) cfg.loopParallelCfg.exclude.push_back(*name);
            }
        }
        if (auto benchIter = (*lp)["benchmark_iterations"].value<int64_t>())
            cfg.loopParallelCfg.benchmarkIterations = static_cast<int>(*benchIter);
        if (auto benchWarm = (*lp)["benchmark_warmup"].value<int64_t>())
            cfg.loopParallelCfg.benchmarkWarmup = static_cast<int>(*benchWarm);
        if (auto part = (*lp)["partition"].value<bool>()) cfg.loopParallelCfg.partitionEnabled = *part;
        if (auto red = (*lp)["reduction"].value<bool>()) cfg.loopParallelCfg.reductionEnabled = *red;
        if (auto strat = (*lp)["partition_strategy"].value<std::string>())
            cfg.loopParallelCfg.partitionStrategy = parseLoopPartitionStrategy(*strat);
        // Note: `min_trip_count` was removed — Topo passes do not gate
        // on workload-side cost heuristics. Presence in Topo.toml emits a
        // deprecation warning via warnUnknownKeys.
        if (auto chunk = (*lp)["chunk_size"].value<int64_t>()) cfg.loopParallelCfg.chunkSize = static_cast<int>(*chunk);
        if (auto inst = (*lp)["instrument"].value<bool>()) cfg.loopParallelCfg.instrument = *inst;
    }

    // [containment] section
    if (auto* cont = tbl["containment"].as_table()) {
        warnUnknownKeys(*cont, "[containment]", {"mode"});
        cfg.containmentCfg.mode = parseMode(*cont, "[containment]");
    }

    // [types] section — user overrides for abstract type bindings
    if (auto* types = tbl["types"].as_table()) {
        for (const auto& [key, val] : *types) {
            if (auto str = val.value<std::string>()) cfg.typeBindings[std::string(key)] = *str;
        }
    }

    // [profile.*] sections
    if (auto* profileSec = tbl["profile"].as_table()) {
        static const std::set<std::string> knownProfileKeys = {"extends",
                                                               "mode",
                                                               "opt_level",
                                                               "obfuscation",
                                                               "obfuscation_salt",
                                                               "embed_ir",
                                                               "parallel",
                                                               "adaptive",
                                                               "data_layout",
                                                               "indirection",
                                                               "observability",
                                                               "observability_exporter",
                                                               "lifetime",
                                                               "loop_parallel",
                                                               "prefetch",
                                                               "type_narrowing",
                                                               "containment",
                                                               "parallel_instrument"};
        for (const auto& [name, val] : *profileSec) {
            if (auto* ptbl = val.as_table()) {
                warnUnknownKeys(*ptbl, "[profile." + std::string(name) + "]", knownProfileKeys);

                build::ProfileOverrides p;
                if (auto ext = (*ptbl)["extends"].value<std::string>()) p.extends = *ext;
                if (auto mode = (*ptbl)["mode"].value<std::string>()) {
                    if (*mode == "aggressive")
                        p.buildMode = BuildMode::Aggressive;
                    else if (*mode == "dev")
                        p.buildMode = BuildMode::Dev;
                }
                if (auto opt = (*ptbl)["opt_level"].value<int64_t>()) {
                    if (*opt >= 0 && *opt <= 3) p.optLevel = static_cast<OptLevel>(*opt);
                }
                if (auto obf = (*ptbl)["obfuscation"].value<std::string>()) {
                    if (*obf == "salted")
                        p.obfMode = ObfuscationMode::Salted;
                    else if (*obf == "normal")
                        p.obfMode = ObfuscationMode::Normal;
                }
                if (auto salt = (*ptbl)["obfuscation_salt"].value<std::string>()) p.obfSalt = *salt;
                if (auto embed = (*ptbl)["embed_ir"].value<bool>()) p.embedIR = *embed;
                if (auto mode = (*ptbl)["parallel"].value<std::string>()) p.parallel = parseFeatureMode(*mode);
                if (auto mode = (*ptbl)["adaptive"].value<std::string>()) p.adaptive = parseFeatureMode(*mode);
                if (auto mode = (*ptbl)["data_layout"].value<std::string>()) p.dataLayout = parseFeatureMode(*mode);
                if (auto mode = (*ptbl)["indirection"].value<std::string>()) p.indirection = parseFeatureMode(*mode);
                if (auto mode = (*ptbl)["observability"].value<std::string>())
                    p.observability = parseFeatureMode(*mode);
                if (auto exp = (*ptbl)["observability_exporter"].value<std::string>()) p.observabilityExporter = *exp;
                if (auto mode = (*ptbl)["lifetime"].value<std::string>()) p.lifetime = parseFeatureMode(*mode);
                if (auto mode = (*ptbl)["loop_parallel"].value<std::string>()) p.loopParallel = parseFeatureMode(*mode);
                if (auto mode = (*ptbl)["prefetch"].value<std::string>()) p.prefetch = parseFeatureMode(*mode);
                if (auto mode = (*ptbl)["type_narrowing"].value<std::string>()) p.typeNarrowing = parseFeatureMode(*mode);
                if (auto mode = (*ptbl)["containment"].value<std::string>()) p.containment = parseFeatureMode(*mode);
                if (auto inst = (*ptbl)["parallel_instrument"].value<bool>()) p.parallelInstrument = *inst;

                cfg.profiles[std::string(name)] = p;
            }
        }
    }

    // Apply selected profile (from --profile CLI flag) before validation
    if (!resolveAndApplyProfile(cfg)) return false;

    // Validate configuration (collect all errors at once)
    {
        // Capture raw TOML strings for enum validation
        if (auto lang = tbl["build"]["language"].value<std::string>()) cfg.rawLanguage = *lang;
        if (auto otype = tbl["build"]["output_type"].value<std::string>()) cfg.rawOutputType = *otype;
        if (auto mode = tbl["builder"]["mode"].value<std::string>()) cfg.rawBuilderMode = *mode;
        auto validation = topo::validateConfig(cfg);

        for (const auto& err : validation.errors) {
            if (err.level == topo::ConfigErrorLevel::Error) {
                std::cerr << "error: " << err.message << "\n";
            } else {
                std::cerr << "warning: " << err.message << "\n";
            }
        }

        if (validation.hasErrors()) return false;
    }

    // [project].name — informational
    if (auto name = tbl["project"]["name"].value<std::string>()) {
        std::cerr << "Project: " << *name << "\n";
    }

    return true;
}

void autoExtension(std::string& outputPath, OutputType outputType) {
    namespace plat = platform;

    auto hasSuffix = [&](std::string_view ext) {
        return platform::endsWith(outputPath, std::string(ext));
    };

    switch (outputType) {
    case OutputType::Exe:
        // A JVM target is an Exe-type whose artifact is a `.jar` — a complete,
        // self-describing name to which the native exe suffix is never correct.
        // Without this guard, Windows (ExeSuffix == ".exe") writes `foo.jar.exe`,
        // which `java -jar` cannot run and which mislocates the .topo-passes
        // sidecar. Native cpp/rust exes never end in `.jar`, so they are
        // unaffected. (On POSIX ExeSuffix is empty, so this case is a no-op.)
        if (!plat::ExeSuffix.empty() && !hasSuffix(plat::ExeSuffix) && !hasSuffix(".jar"))
            outputPath += std::string(plat::ExeSuffix);
        break;
    case OutputType::Shared:
        if (!hasSuffix(plat::SharedLibSuffix)) outputPath += std::string(plat::SharedLibSuffix);
        break;
    case OutputType::Static:
        if (!hasSuffix(plat::StaticLibSuffix)) outputPath += std::string(plat::StaticLibSuffix);
        break;
    }
}

} // namespace topo::build
