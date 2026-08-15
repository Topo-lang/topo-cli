// MixedAnalysisProvider — composes the cpp and rust analysis providers
// for mixed C++/Rust projects. See the header for the composition
// contract.

#include "MixedAnalysisProvider.h"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace fs = std::filesystem;

namespace topo::check {

namespace {

bool isRustFile(const std::string& filePath) {
    return fs::path(filePath).extension() == ".rs";
}

// Dispatch-by-extension extractor wrappers. Stateless beyond the two
// inner pointers: CheckRunner's parallelMap already calls one extractor
// object concurrently across files, and these wrappers inherit that
// thread-safety contract from the inners they delegate to.

class MixedSymbolExtractor : public SymbolExtractor {
public:
    MixedSymbolExtractor(std::unique_ptr<SymbolExtractor> cpp,
                         std::unique_ptr<SymbolExtractor> rust)
        : cpp_(std::move(cpp)), rust_(std::move(rust)) {}

    std::vector<HostSymbol> extractSymbols(const std::string& filePath) override {
        return (isRustFile(filePath) ? rust_ : cpp_)->extractSymbols(filePath);
    }

private:
    std::unique_ptr<SymbolExtractor> cpp_;
    std::unique_ptr<SymbolExtractor> rust_;
};

class MixedImportExtractor : public ImportExtractor {
public:
    MixedImportExtractor(std::unique_ptr<ImportExtractor> cpp,
                         std::unique_ptr<ImportExtractor> rust)
        : cpp_(std::move(cpp)), rust_(std::move(rust)) {}

    std::vector<HostImport> extractImports(const std::string& filePath) override {
        return (isRustFile(filePath) ? rust_ : cpp_)->extractImports(filePath);
    }

private:
    std::unique_ptr<ImportExtractor> cpp_;
    std::unique_ptr<ImportExtractor> rust_;
};

class MixedCallSiteExtractor : public CallSiteExtractor {
public:
    MixedCallSiteExtractor(std::unique_ptr<CallSiteExtractor> cpp,
                           std::unique_ptr<CallSiteExtractor> rust)
        : cpp_(std::move(cpp)), rust_(std::move(rust)) {}

    std::vector<DetectedCallSite> extractCallSites(const std::string& filePath) override {
        return (isRustFile(filePath) ? rust_ : cpp_)->extractCallSites(filePath);
    }

private:
    std::unique_ptr<CallSiteExtractor> cpp_;
    std::unique_ptr<CallSiteExtractor> rust_;
};

class MixedCallEdgeExtractor : public CallEdgeExtractor {
public:
    MixedCallEdgeExtractor(std::unique_ptr<CallEdgeExtractor> cpp,
                           std::unique_ptr<CallEdgeExtractor> rust)
        : cpp_(std::move(cpp)), rust_(std::move(rust)) {}

    std::vector<CallEdge> extractCallEdges(const std::string& filePath) override {
        return (isRustFile(filePath) ? rust_ : cpp_)->extractCallEdges(filePath);
    }

private:
    std::unique_ptr<CallEdgeExtractor> cpp_;
    std::unique_ptr<CallEdgeExtractor> rust_;
};

class MixedSymbolAccessExtractor : public SymbolAccessExtractor {
public:
    MixedSymbolAccessExtractor(std::unique_ptr<SymbolAccessExtractor> cpp,
                               std::unique_ptr<SymbolAccessExtractor> rust)
        : cpp_(std::move(cpp)), rust_(std::move(rust)) {}

    std::vector<SymbolAccess> extractSymbolAccesses(const std::string& filePath) override {
        return (isRustFile(filePath) ? rust_ : cpp_)->extractSymbolAccesses(filePath);
    }

private:
    std::unique_ptr<SymbolAccessExtractor> cpp_;
    std::unique_ptr<SymbolAccessExtractor> rust_;
};

// A wrapper covering only one half would silently under-check the other —
// when either inner factory yields nothing, return nullptr so CheckRunner's
// existing "extractor unavailable" error path fires instead.
template <typename Wrapper, typename Inner>
std::unique_ptr<Inner> composeOrNull(std::unique_ptr<Inner> cpp, std::unique_ptr<Inner> rust) {
    if (!cpp || !rust) return nullptr;
    return std::make_unique<Wrapper>(std::move(cpp), std::move(rust));
}

} // anonymous namespace

MixedAnalysisProvider::MixedAnalysisProvider(std::unique_ptr<LanguageAnalysisProvider> cppInner,
                                             std::unique_ptr<LanguageAnalysisProvider> rustInner,
                                             std::string rustCrateDir)
    : cpp_(std::move(cppInner)), rust_(std::move(rustInner)),
      rustCrateDir_(std::move(rustCrateDir)) {}

std::unique_ptr<SymbolExtractor> MixedAnalysisProvider::createSymbolExtractor() {
    return composeOrNull<MixedSymbolExtractor>(cpp_->createSymbolExtractor(),
                                               rust_->createSymbolExtractor());
}

std::unique_ptr<ImportExtractor> MixedAnalysisProvider::createImportExtractor() {
    return composeOrNull<MixedImportExtractor>(cpp_->createImportExtractor(),
                                               rust_->createImportExtractor());
}

std::unique_ptr<CallSiteExtractor> MixedAnalysisProvider::createCallSiteExtractor() {
    return composeOrNull<MixedCallSiteExtractor>(cpp_->createCallSiteExtractor(),
                                                 rust_->createCallSiteExtractor());
}

std::unique_ptr<CallEdgeExtractor> MixedAnalysisProvider::createCallEdgeExtractor() {
    return composeOrNull<MixedCallEdgeExtractor>(cpp_->createCallEdgeExtractor(),
                                                 rust_->createCallEdgeExtractor());
}

std::unique_ptr<SymbolAccessExtractor> MixedAnalysisProvider::createSymbolAccessExtractor() {
    return composeOrNull<MixedSymbolAccessExtractor>(cpp_->createSymbolAccessExtractor(),
                                                     rust_->createSymbolAccessExtractor());
}

std::vector<std::string> MixedAnalysisProvider::collectSourceFiles(
    const std::string& projectDir,
    const std::vector<std::string>& includeDirs) const {
    // cpp half: project scan plus the [build.cpp] source/include paths the
    // runner resolved into includeDirs (the cpp provider handles
    // regular-file entries there). rust half: the rust provider scans its
    // project directory only and ignores includeDirs, so root it at the
    // crate directory derived from [build.rust].manifest when present.
    auto files = cpp_->collectSourceFiles(projectDir, includeDirs);
    const std::string& rustRoot = rustCrateDir_.empty() ? projectDir : rustCrateDir_;
    auto rustFiles = rust_->collectSourceFiles(rustRoot, {});
    files.insert(files.end(), rustFiles.begin(), rustFiles.end());

    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

} // namespace topo::check
