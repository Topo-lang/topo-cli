#ifndef TOPO_CHECK_CHECKRUNNER_H
#define TOPO_CHECK_CHECKRUNNER_H

#include "topo/Basic/HostLanguage.h"
#include "topo/Build/PassConfig.h"
#include "topo/Check/CheckTypes.h"
#include "topo/Check/CompletenessCheck.h"
#include "topo/Check/LanguageAnalysisProvider.h"
#include "topo/Sema/SymbolTable.h"
#include "topo/Sema/VisibilityCollector.h"

#include <memory>
#include <string>
#include <vector>

namespace topo {

struct CheckConfig {
    std::string projectDir = ".";
    std::string checkName = "all"; // completeness|containment|import-path|purity|visibility|stage-isolation|all
    std::string filter;
    bool verbose = false;
    bool jsonOutput = false;
    bool listUnsafe = false;        // --list-unsafe
    bool deepMode = false;          // --deep: use L2 LSP analysis
    int jobs = 0;                   // --jobs N: 0 = auto (hardware_concurrency capped, max 16), 1 = sequential
    bool jobsExplicit = false;      // CLI flag set — overrides [check].jobs in Topo.toml
};

class CheckRunner {
public:
    explicit CheckRunner(const CheckConfig& config);
    ~CheckRunner();

    /// Load Topo.toml configuration. Returns false on error.
    bool loadConfig();

    /// Run all requested checks. Returns 0 on pass, 1 on errors.
    int run();

    /// Access detailed results from the last run() call.
    const std::vector<std::pair<std::string, check::CheckResult>>& lastResults() const {
        return lastResults_;
    }

    /// List all detected unsafe points. Returns count of unsafe points found.
    int listUnsafePoints();

private:
    bool tryCacheHit(int& cachedResult);
    void saveCheckCache(int result);
    std::vector<std::string> discoverRelevantFiles() const;
    static std::string hashFile(const std::string& path);

    bool runFrontend();
    check::CheckResult runCompleteness();
    check::CheckResult runContainment();
    check::CheckResult runImportPath();
    check::CheckResult runPurity();
    check::CheckResult runVisibility();
    check::CheckResult runStageIsolation();

    std::vector<std::string> collectSourceFiles() const;
    bool matchesFilter(const std::string& checkName) const;
    void reportResult(const std::string& checkName, const check::CheckResult& result);
    void reportResultJson(const std::string& checkName, const check::CheckResult& result);

    CheckConfig config_;

    // Loaded from Topo.toml
    std::string topoRoot_;
    HostLanguage language_ = HostLanguage::Cpp;
    std::vector<std::string> includeDirs_;
    check::CompletenessConfig completenessCfg_;
    ContainmentConfig containmentCfg_;
    PurityConfig purityCfg_;
    VisibilityCheckConfig visibilityCfg_;
    StageIsolationConfig stageIsolationCfg_;

    // Language-specific analysis provider (created in loadConfig)
    std::unique_ptr<check::LanguageAnalysisProvider> provider_;

    // Frontend results (populated by runFrontend)
    SymbolTable symbols_;
    std::vector<VisibilityEntry> visEntries_;
    bool frontendDone_ = false;

    // Detailed results from last run()
    std::vector<std::pair<std::string, check::CheckResult>> lastResults_;

};

} // namespace topo

#endif // TOPO_CHECK_CHECKRUNNER_H
