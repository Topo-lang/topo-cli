// topo-check — standalone declaration checker CLI
//
// Verifies user code against .topo declarations at the source level.
// Zero LLVM dependency. Suitable for pre-commit hooks.

#include "CheckRunner.h"

// Plugin headers are conditionally included so a build with a subset
// of language plugins (e.g. when topo-lang-cpp's LLVM scope is gated
// OFF upstream and TopoCppPlugin is unavailable) still produces a
// valid topo-check binary.
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

#include <cstring>
#include <iostream>

namespace {

void printUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [options]\n"
              << "\n"
              << "  --project <dir>      Project directory (default: current directory)\n"
              << "  --check <name>       Check to run:\n"
              << "                         completeness, containment, import-path,\n"
              << "                         purity, visibility, stage-isolation, all\n"
              << "                       (default: all)\n"
              << "  --filter <pattern>   Substring filter on check names\n"
              << "  --deep               Use L2 clangd-based analysis for containment\n"
              << "  --verbose            Verbose output\n"
              << "  --json               JSON output format\n"
              << "  --list-unsafe        List all detected unsafe points with levels\n"
              << "  --jobs N             Parallel worker count (0=auto, 1=sequential; overrides [check].jobs)\n"
              << "  --help               Show this help\n"
              << "\n"
              << "Return codes:\n"
              << "  0  All checks passed\n"
              << "  1  One or more checks found errors\n"
              << "  2  Configuration or parse error\n";
}

bool parseArgs(int argc, char* argv[], topo::CheckConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--project") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --project requires an argument\n";
                return false;
            }
            cfg.projectDir = argv[++i];
        } else if (std::strcmp(argv[i], "--check") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --check requires an argument\n";
                return false;
            }
            cfg.checkName = argv[++i];
            // Validate check name
            if (cfg.checkName != "completeness" && cfg.checkName != "containment" &&
                cfg.checkName != "import-path" && cfg.checkName != "purity" &&
                cfg.checkName != "visibility" && cfg.checkName != "stage-isolation" &&
                cfg.checkName != "all") {
                std::cerr << "Error: unknown check '" << cfg.checkName << "'\n"
                          << "Valid checks: completeness, containment, import-path, "
                             "purity, visibility, stage-isolation, all\n";
                return false;
            }
        } else if (std::strcmp(argv[i], "--filter") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --filter requires an argument\n";
                return false;
            }
            cfg.filter = argv[++i];
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            cfg.verbose = true;
        } else if (std::strcmp(argv[i], "--json") == 0) {
            cfg.jsonOutput = true;
        } else if (std::strcmp(argv[i], "--deep") == 0) {
            cfg.deepMode = true;
        } else if (std::strcmp(argv[i], "--list-unsafe") == 0) {
            cfg.listUnsafe = true;
        } else if (std::strcmp(argv[i], "--jobs") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --jobs requires an argument\n";
                return false;
            }
            try {
                int n = std::stoi(argv[++i]);
                if (n < 0) {
                    std::cerr << "Error: --jobs must be >= 0\n";
                    return false;
                }
                cfg.jobs = n;
                cfg.jobsExplicit = true;
            } catch (...) {
                std::cerr << "Error: --jobs requires an integer argument\n";
                return false;
            }
        } else if (std::strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Error: unknown argument '" << argv[i] << "'\n";
            printUsage(argv[0]);
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
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

    topo::CheckConfig cfg;
    if (!parseArgs(argc, argv, cfg)) return 2;

    topo::CheckRunner runner(cfg);
    if (!runner.loadConfig()) return 2;

    if (cfg.listUnsafe) {
        return runner.listUnsafePoints() >= 0 ? 0 : 1;
    }

    return runner.run();
}
