// topo-check — standalone declaration checker CLI
//
// Verifies user code against .topo declarations at the source level.
// Zero LLVM dependency. Suitable for pre-commit hooks.

#include "CheckRunner.h"
#include "AuditSupport.h"

// Plugin headers are conditionally included so a build with a subset
// of language plugins (an upstream language package absent from the
// prefix) still produces a valid topo-check binary.
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
              << "  --java-project <file> Java source/classpath model (JSON)\n"
              << "  --cpp-project <file>  Selected C++ compile_commands.json\n"
              << "  --build-model <file>  Resolved topo-build configuration\n"
              << "  --audit              Require complete semantic audit and manual approvals\n"
              << "  --audit-policy <file> Audit policy (default: Topo.audit.toml if present)\n"
              << "  --audit-report <file> Write the full versioned audit report\n"
              << "  --analysis-input-root <dir> Allow audit reads of an external input root (repeatable)\n"
              << "  --audit-cache <dir>   Reuse facts in a protected reviewer directory outside the project\n"
              << "  --native-evidence <file> Compare FFM bindings with a selected native manifest\n"
              << "  --evidence-root ID=DIR  Map a manifest root to reviewed local files\n"
              << "  --trust-store <file> User-controlled trust store outside the subject project\n"
              << "  --accept-review <file> Accept the current manual boundaries in a saved report\n"
              << "  --accept-dependency <key> Accept a dependency from --audit-report\n"
              << "  --trust-scope project|user  Dependency approval scope (default: project)\n"
              << "  --revoke-review <key> Revoke an approval key\n"
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
        std::string* auditValue = nullptr;
        if (std::strcmp(argv[i], "--audit-policy") == 0) auditValue = &cfg.auditPolicyPath;
        if (std::strcmp(argv[i], "--audit-report") == 0) auditValue = &cfg.auditReportPath;
        if (std::strcmp(argv[i], "--audit-cache") == 0) auditValue = &cfg.auditCacheDir;
        if (std::strcmp(argv[i], "--native-evidence") == 0) auditValue = &cfg.nativeEvidencePath;
        if (std::strcmp(argv[i], "--cpp-project") == 0) auditValue = &cfg.cppProjectPath;
        if (std::strcmp(argv[i], "--build-model") == 0) auditValue = &cfg.buildModelPath;
        if (std::strcmp(argv[i], "--trust-store") == 0) auditValue = &cfg.trustStore;
        if (std::strcmp(argv[i], "--trust-scope") == 0) auditValue = &cfg.trustScope;
        if (std::strcmp(argv[i], "--accept-review") == 0) auditValue = &cfg.acceptReview;
        if (std::strcmp(argv[i], "--accept-dependency") == 0) auditValue = &cfg.acceptDependency;
        if (std::strcmp(argv[i], "--revoke-review") == 0) auditValue = &cfg.revokeReview;
        if (auditValue) {
            if (i + 1 >= argc) { std::cerr << "Error: " << argv[i] << " requires a value\n"; return false; }
            *auditValue = argv[++i];
        } else if (std::strcmp(argv[i], "--analysis-input-root") == 0) {
            if (i + 1 >= argc) { std::cerr << "Error: --analysis-input-root requires a directory\n"; return false; }
            cfg.analysisInputRoots.emplace_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--evidence-root") == 0) {
            if (i+1>=argc) { std::cerr << "Error: --evidence-root requires ID=DIR\n"; return false; }
            std::string entry=argv[++i]; auto at=entry.find('=');
            if (at==std::string::npos || !at || at+1==entry.size() || !cfg.evidenceRoots.emplace(entry.substr(0,at),entry.substr(at+1)).second) {
                std::cerr << "Error: invalid or duplicate evidence root\n"; return false;
            }
        } else if (std::strcmp(argv[i], "--audit") == 0) {
            cfg.audit = true;
        } else if (std::strcmp(argv[i], "--project") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --project requires an argument\n";
                return false;
            }
            cfg.projectDir = argv[++i];
        } else if (std::strcmp(argv[i], "--java-project") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --java-project requires a file\n";
                return false;
            }
            cfg.javaProjectPath = argv[++i];
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
    const int actions = !cfg.acceptReview.empty() + !cfg.acceptDependency.empty() + !cfg.revokeReview.empty();
    if (actions > 1 || (actions && cfg.audit) ||
        (!cfg.acceptDependency.empty() && cfg.auditReportPath.empty()) ||
        (cfg.trustScope != "project" && cfg.trustScope != "user") ||
        (cfg.audit && (cfg.listUnsafe || cfg.checkName != "all" || !cfg.filter.empty()))) {
        std::cerr << "Error: use one trust action, or --audit with all checks; dependency acceptance requires --audit-report\n";
        return false;
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

    if (!cfg.acceptReview.empty() || !cfg.acceptDependency.empty() || !cfg.revokeReview.empty())
        return topo::updateAuditTrust(cfg);

    try {
    topo::CheckRunner runner(cfg);
    if (!runner.loadConfig()) return 2;

    if (cfg.listUnsafe) {
        return runner.listUnsafePoints() >= 0 ? 0 : 1;
    }

    return runner.run(); }
    catch (const std::exception& e) { std::cerr << "topo-check: " << e.what() << '\n'; return 2; }
}
