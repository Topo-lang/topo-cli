#ifndef TOPO_CLI_CHECK_MIXEDANALYSISPROVIDER_H
#define TOPO_CLI_CHECK_MIXEDANALYSISPROVIDER_H

#include "topo/Check/LanguageAnalysisProvider.h"

#include <memory>
#include <string>
#include <vector>

namespace topo::check {

/// Composite analysis provider for mixed C++/Rust projects.
///
/// Every check in CheckRunner consumes exactly one provider, so composing
/// at this seam makes completeness / containment / purity / visibility /
/// stage-isolation (and the incremental cache's file discovery) mixed-aware
/// with no orchestration changes: collectSourceFiles unions both halves'
/// scans and the extractor factories return dispatch-by-extension wrappers
/// (".rs" → rust inner, everything else → cpp inner).
///
/// Inherited defaults are deliberate: separator() — both inner languages
/// use "::"; initLSP() false / runDeepContainment() nullopt — L2 is
/// unsupported for mixed in v1, so --deep takes CheckRunner's loud
/// no-silent-degradation exit instead of half-depth analysis.
///
/// Internal to TopoCheckRunner; not installed.
class MixedAnalysisProvider : public LanguageAnalysisProvider {
public:
    /// `rustCrateDir` roots the rust half's source scan (derived from
    /// [build.rust].manifest); empty falls back to the project directory.
    MixedAnalysisProvider(std::unique_ptr<LanguageAnalysisProvider> cppInner,
                          std::unique_ptr<LanguageAnalysisProvider> rustInner,
                          std::string rustCrateDir);

    std::unique_ptr<SymbolExtractor> createSymbolExtractor() override;
    std::unique_ptr<ImportExtractor> createImportExtractor() override;
    std::unique_ptr<CallSiteExtractor> createCallSiteExtractor() override;
    std::unique_ptr<CallEdgeExtractor> createCallEdgeExtractor() override;
    std::unique_ptr<SymbolAccessExtractor> createSymbolAccessExtractor() override;

    std::vector<std::string> collectSourceFiles(
        const std::string& projectDir,
        const std::vector<std::string>& includeDirs) const override;

private:
    std::unique_ptr<LanguageAnalysisProvider> cpp_;
    std::unique_ptr<LanguageAnalysisProvider> rust_;
    std::string rustCrateDir_;
};

} // namespace topo::check

#endif // TOPO_CLI_CHECK_MIXEDANALYSISPROVIDER_H
