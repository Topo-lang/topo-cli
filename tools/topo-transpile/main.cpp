#include "topo/Basic/HostLanguage.h"
#include "topo/Transpile/TranspileDriver.h"
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

#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

static void printUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [options] <file.topo>\n"
              << "\nOptions:\n"
              << "  --from <lang>       Source language (cpp/rust/java/python/typescript)\n"
              << "                      or 'topo' for .topo-source mode (the .topo file\n"
              << "                      itself is the source: composite bodies from logic\n"
              << "                      blocks, leaf bodies from adapters)\n"
              << "  --to <lang>         Target language (cpp/rust/java/python/typescript)\n"
              << "  --sources <files>   Source files to extract from (comma-separated).\n"
              << "                      Required for host-source mode; ignored for --from topo\n"
              << "  --adapters <file>   topo-app adapter manifest (JSON); .topo-source mode\n"
              << "                      only. The builtin adapter source is always used\n"
              << "  --output <dir>      Output directory (default: ./transpiled/)\n"
              << "  --functions <names> Functions to transpile (comma-separated, default: all)\n"
              << "  --pipeline <name>   Only include functions reachable from the named pipeline\n"
              << "  --verify[=N]        Gate on post-transpile verification: fail if more than\n"
              << "                      N unsupported constructs remain (bare --verify => N=0,\n"
              << "                      i.e. strict). Without this flag the legacy default\n"
              << "                      applies (any unsupported construct => exit 1).\n"
              << "  --verify-strict     Equivalent to --verify=0 (any unsupported => failure,\n"
              << "                      surfaced as a verification error).\n"
              << "  --help              Show this help\n"
              << "\nExamples:\n"
              << "  topo-transpile --from cpp --to rust --sources src/main.cpp api.topo\n"
              << "  topo-transpile --from java --to cpp --sources Order.java --pipeline process_order api.topo\n"
              << "  topo-transpile --from topo --to rust app.topo\n";
}

/// Whether `lang` is a host language topo-transpile accepts for --from/--to.
/// `topo::parseHostLanguage` silently maps any unknown name to Cpp, so an
/// unvalidated `--from xyz` would be transpiled as if it were C++ instead of
/// being rejected. The accepted set mirrors the documented usage string
/// (cpp/rust/java/python/typescript); "mixed" and "topo" are NOT host
/// languages here and are handled separately by the caller.
static bool isSupportedHostLanguage(const std::string& lang) {
    return lang == "cpp" || lang == "rust" || lang == "java" ||
           lang == "python" || lang == "typescript";
}

/// Split a comma-separated string into a vector of trimmed tokens.
static std::vector<std::string> splitComma(const std::string& input) {
    std::vector<std::string> result;
    std::istringstream stream(input);
    std::string token;
    while (std::getline(stream, token, ',')) {
        // Trim leading/trailing whitespace
        size_t start = token.find_first_not_of(" \t");
        size_t end = token.find_last_not_of(" \t");
        if (start != std::string::npos) {
            result.push_back(token.substr(start, end - start + 1));
        }
    }
    return result;
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

    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::string topoFile;
    std::string fromLang;
    std::string toLang;
    std::string sourcesStr;
    std::string adaptersPath;
    std::string outputDir = "./transpiled/";
    std::string functionsStr;
    std::string pipelineName;
    std::optional<int> verifyMaxUnsupported; // nullopt = legacy default gate

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }

        if (arg == "--from") {
            if (i + 1 >= argc) {
                std::cerr << "error: --from requires a language argument\n";
                return 1;
            }
            fromLang = argv[++i];
            continue;
        }
        if (arg == "--to") {
            if (i + 1 >= argc) {
                std::cerr << "error: --to requires a language argument\n";
                return 1;
            }
            toLang = argv[++i];
            continue;
        }
        if (arg == "--sources") {
            if (i + 1 >= argc) {
                std::cerr << "error: --sources requires a file list\n";
                return 1;
            }
            sourcesStr = argv[++i];
            continue;
        }
        if (arg == "--adapters") {
            if (i + 1 >= argc) {
                std::cerr << "error: --adapters requires a manifest path\n";
                return 1;
            }
            adaptersPath = argv[++i];
            continue;
        }
        if (arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "error: --output requires a directory path\n";
                return 1;
            }
            outputDir = argv[++i];
            continue;
        }
        if (arg == "--functions") {
            if (i + 1 >= argc) {
                std::cerr << "error: --functions requires a name list\n";
                return 1;
            }
            functionsStr = argv[++i];
            continue;
        }
        if (arg == "--pipeline") {
            if (i + 1 >= argc) {
                std::cerr << "error: --pipeline requires a pipeline name\n";
                return 1;
            }
            pipelineName = argv[++i];
            continue;
        }
        if (arg == "--verify-strict") {
            verifyMaxUnsupported = 0;
            continue;
        }
        if (arg == "--verify") {
            verifyMaxUnsupported = 0; // bare => strict
            continue;
        }
        if (arg.rfind("--verify=", 0) == 0) {
            std::string val = arg.substr(std::string("--verify=").size());
            try {
                size_t pos = 0;
                int n = std::stoi(val, &pos);
                if (pos != val.size() || n < 0) throw std::invalid_argument(val);
                verifyMaxUnsupported = n;
            } catch (const std::exception&) {
                std::cerr << "error: --verify expects a non-negative integer, got '"
                          << val << "'\n";
                return 1;
            }
            continue;
        }
        if (arg[0] == '-') {
            std::cerr << "error: unknown option '" << arg << "'\n";
            printUsage(argv[0]);
            return 1;
        }

        // Positional: .topo file
        topoFile = arg;
    }

    // --- Validate required arguments ---

    if (topoFile.empty()) {
        std::cerr << "error: no .topo input file specified\n";
        printUsage(argv[0]);
        return 1;
    }
    if (!std::filesystem::exists(topoFile)) {
        std::cerr << "error: file not found: " << topoFile << "\n";
        return 1;
    }
    if (fromLang.empty()) {
        std::cerr << "error: --from is required\n";
        printUsage(argv[0]);
        return 1;
    }
    if (toLang.empty()) {
        std::cerr << "error: --to is required\n";
        printUsage(argv[0]);
        return 1;
    }

    // `--from topo` selects the .topo-source mode (M4): the .topo file itself
    // is the source. The host-source extractor is bypassed; --sources is not
    // required and is ignored if given.
    const bool fromTopoSource = (fromLang == "topo");

    // Reject unknown languages explicitly. parseHostLanguage() defaults any
    // unrecognized name to C++, which would silently transpile e.g.
    // `--from xyz` as C++ rather than reporting the typo.
    if (!fromTopoSource && !isSupportedHostLanguage(fromLang)) {
        std::cerr << "error: unsupported --from language '" << fromLang
                  << "' (expected cpp/rust/java/python/typescript or topo)\n";
        return 1;
    }
    if (!isSupportedHostLanguage(toLang)) {
        std::cerr << "error: unsupported --to language '" << toLang
                  << "' (expected cpp/rust/java/python/typescript)\n";
        return 1;
    }

    if (!fromTopoSource && fromLang == toLang) {
        std::cerr << "error: source and target languages must differ\n";
        return 1;
    }
    if (!fromTopoSource && sourcesStr.empty()) {
        std::cerr << "error: --sources is required\n";
        printUsage(argv[0]);
        return 1;
    }
    if (fromTopoSource && !adaptersPath.empty() &&
        !std::filesystem::exists(adaptersPath)) {
        std::cerr << "error: adapter manifest not found: " << adaptersPath << "\n";
        return 1;
    }

    // --- Build request ---

    topo::transpile::TranspileRequest request;
    request.topoFile = topoFile;
    request.fromTopoSource = fromTopoSource;
    // In .topo-source mode the source language is the .topo file itself, not
    // a host language; sourceLanguage is left at its default and ignored.
    request.sourceLanguage =
        fromTopoSource ? topo::HostLanguage::Cpp : topo::parseHostLanguage(fromLang);
    request.targetLanguage = topo::parseHostLanguage(toLang);
    request.adapterManifestPath = adaptersPath;
    request.outputDir = outputDir;

    for (const auto& s : splitComma(sourcesStr)) {
        request.sourceFiles.emplace_back(s);
    }

    if (!functionsStr.empty()) {
        request.functions = splitComma(functionsStr);
    }

    request.pipelineName = pipelineName;
    request.verifyMaxUnsupported = verifyMaxUnsupported;

    // --- Run ---

    topo::transpile::TranspileDriver driver;
    auto result = driver.run(request);

    // Print errors (target-language capability issues)
    for (const auto& e : result.errors) {
        std::cerr << "error: " << e << "\n";
    }

    // Print warnings
    for (const auto& w : result.warnings) {
        std::cerr << "warning: " << w << "\n";
    }

    // Structured post-transpile verification summary (always printed once
    // emission produced a module, regardless of gate configuration).
    {
        const auto& v = result.verification;
        const auto& fb = v.fidelity;
        std::cerr << "verification: " << v.totalUnsupported
                  << " unsupported construct(s) across "
                  << v.perFunction.size() << " function(s); fidelity "
                  << "source=" << fb.source
                  << " recovered=" << fb.recovered
                  << " inferred=" << fb.inferred << "\n";
        for (const auto& fu : v.perFunction) {
            std::cerr << "  " << fu.qualifiedName << ": ";
            for (size_t k = 0; k < fu.constructs.size(); ++k) {
                std::cerr << (k ? ", " : "") << fu.constructs[k];
            }
            std::cerr << "\n";
        }
    }

    if (!result.success) {
        std::cerr << "transpilation failed\n";
        return 1;
    }

    // Print summary
    for (const auto& f : result.outputFiles) {
        std::cout << "wrote: " << f << "\n";
    }

    // Legacy default exit policy: when the user did NOT pass any --verify
    // flag, preserve the historical behaviour where any unsupported
    // construct yields exit 1. When a --verify flag was given, the driver
    // already enforced the configured tolerance via result.success above,
    // so we do not double-gate here.
    if (!verifyMaxUnsupported.has_value() && result.unsupportedCount > 0) {
        std::cerr << result.unsupportedCount << " unsupported construct(s) in output\n";
        return 1;
    }

    return 0;
}
