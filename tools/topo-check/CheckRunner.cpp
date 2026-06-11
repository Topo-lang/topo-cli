// topo-check — CheckRunner implementation
//
// Orchestrates .topo frontend pipeline and checks:
// completeness, containment, import-path.

#include "CheckRunner.h"
#include "MixedAnalysisProvider.h"

#include "topo/Basic/Diagnostic.h"
#include "topo/Basic/HostLanguage.h"
#include "topo/Platform/FileGlob.h"
#include "topo/Check/CompletenessCheck.h"
#include "topo/Check/ContainmentCheck.h"
#include "topo/Check/PurityCheck.h"
#include "topo/Check/StageIsolationCheck.h"
#include "topo/Check/VisibilityCheck.h"
#include "topo/Analysis/ImportPathCheck.h"
#include "topo/Sema/ImportResolver.h"
#include "topo/Sema/SemanticAnalyzer.h"
#include "topo/Sema/VisibilityCollector.h"

// Plugin registry — replaces hardcoded provider factory
#include "topo/Lang/LanguagePlugin.h"

#define TOML_HEADER_ONLY 1
#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>

#include <nlohmann/json.hpp>

#include "topo/Basic/FNVHash.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace topo {

namespace {

// On-disk version for `.topo-check-cache`. Bumped to 3 when `deep` was added
// to the cache key so older (mode-blind) caches invalidate on read. Bumped to
// 4 when requested-deep-but-unavailable became a hard error: before that, a
// degraded L1-grade run could be stamped `deep: true, result: 0`, so any
// version-3 deep verdict is untrustworthy and must re-run.
constexpr int CHECK_CACHE_VERSION = 4;

// Small internal thread pool. File-level workers pull tasks from a FIFO queue.
// Lifetime scoped to the `parallelMap` helper below — workers exit when all
// tasks complete and join on scope exit.
class FilePool {
public:
    explicit FilePool(size_t n) : stopping_(false) {
        workers_.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
    }

    ~FilePool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lk(m_);
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    void wait() {
        std::unique_lock<std::mutex> lk(m_);
        drainCv_.wait(lk, [this]() { return tasks_.empty() && inflight_ == 0; });
    }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this]() { return stopping_ || !tasks_.empty(); });
                if (stopping_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
                ++inflight_;
            }
            try {
                task();
            } catch (...) {
            }
            {
                std::lock_guard<std::mutex> lk(m_);
                --inflight_;
                if (tasks_.empty() && inflight_ == 0) {
                    drainCv_.notify_all();
                }
            }
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex m_;
    std::condition_variable cv_;
    std::condition_variable drainCv_;
    size_t inflight_ = 0;
    bool stopping_;
};

// Parallel map: apply `fn(index, file)` across `files`, returning results in
// input order. Thread count `jobs` honors: 1 = sequential; 0 = hardware_concurrency() capped at 16.
template <typename T, typename Fn>
std::vector<T> parallelMap(const std::vector<std::string>& files, int jobs, Fn fn) {
    std::vector<T> out(files.size());
    if (files.empty()) return out;

    int effective = jobs;
    if (effective <= 0) {
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 1;
        if (hw > 16) hw = 16;
        effective = static_cast<int>(hw);
    }
    if (effective == 1 || files.size() == 1) {
        for (size_t i = 0; i < files.size(); ++i) out[i] = fn(i, files[i]);
        return out;
    }

    size_t workerCount = std::min(static_cast<size_t>(effective), files.size());
    FilePool pool(workerCount);
    std::atomic<size_t> next{0};
    for (size_t w = 0; w < workerCount; ++w) {
        pool.submit([&]() {
            while (true) {
                size_t i = next.fetch_add(1);
                if (i >= files.size()) return;
                out[i] = fn(i, files[i]);
            }
        });
    }
    pool.wait();
    return out;
}

int resolveJobs(int jobs) {
    if (jobs <= 0) {
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 1;
        if (hw > 16) hw = 16;
        return static_cast<int>(hw);
    }
    return jobs;
}

} // namespace

CheckRunner::CheckRunner(const CheckConfig& config) : config_(config) {}

CheckRunner::~CheckRunner() = default;

// ---------------------------------------------------------------------------
// Incremental cache
// ---------------------------------------------------------------------------

std::string CheckRunner::hashFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return "";
    std::string contents((std::istreambuf_iterator<char>(ifs)),
                          std::istreambuf_iterator<char>());
    uint64_t h = fnv1aHash(contents);
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016" PRIx64, h);
    return std::string(buf);
}

std::vector<std::string> CheckRunner::discoverRelevantFiles() const {
    std::vector<std::string> files;

    // Topo.toml
    fs::path tomlPath = fs::path(config_.projectDir) / "Topo.toml";
    if (fs::exists(tomlPath)) {
        std::error_code tomlEc;
        auto canonicalToml = fs::canonical(tomlPath, tomlEc);
        if (!tomlEc) files.push_back(canonicalToml.string());
    }

    // All .topo files under the project directory
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(config_.projectDir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".topo") {
            // Use the non-throwing overload: a dangling symlink or a file
            // removed mid-scan (TOCTOU) must degrade to skipping the entry,
            // not abort the process (main() has no exception barrier).
            std::error_code ec1;
            auto canonicalEntry = fs::canonical(entry.path(), ec1);
            if (!ec1) files.push_back(canonicalEntry.string());
        }
    }

    // Host source files (language-dependent)
    auto srcFiles = collectSourceFiles();
    for (const auto& f : srcFiles) {
        std::error_code ec2;
        auto canonical = fs::canonical(f, ec2);
        if (!ec2) files.push_back(canonical.string());
    }

    // Deduplicate and sort for deterministic comparison
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

bool CheckRunner::tryCacheHit(int& cachedResult) {
    // Only cache for default configuration (all checks, no filter)
    if (config_.checkName != "all" || !config_.filter.empty()) return false;

    fs::path cachePath = fs::path(config_.projectDir) / ".topo-check-cache";
    if (!fs::exists(cachePath)) return false;

    std::ifstream ifs(cachePath);
    if (!ifs) return false;

    nlohmann::json cache;
    // A single guard covers both the parse and the typed accesses below: a
    // corrupted or type-mismatched cache (e.g. a non-object root, or a hash
    // value that is not a string) must degrade to a cache miss, not throw a
    // json::type_error out of an un-try/catch'd main().
    try {
        ifs >> cache;

        if (cache.value("version", 0) != CHECK_CACHE_VERSION) return false;

        // L1 (regex) and L2 (--deep) take different analysis paths and can
        // produce different verdicts. The cache must not be shared across
        // modes, or a deep run silently reuses a stale L1 PASS.
        if (cache.value("deep", false) != config_.deepMode) return false;

        auto cachedFiles = cache.value("files", nlohmann::json::object());
        auto currentFiles = discoverRelevantFiles();

        // File count mismatch → files added or removed
        if (currentFiles.size() != cachedFiles.size()) return false;

        // Verify every file hash matches
        for (const auto& path : currentFiles) {
            auto it = cachedFiles.find(path);
            if (it == cachedFiles.end()) return false;
            if (hashFile(path) != it->get<std::string>()) return false;
        }

        cachedResult = cache.value("result", -1);
        return cachedResult >= 0;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

void CheckRunner::saveCheckCache(int result) {
    // Only cache for default configuration
    if (config_.checkName != "all" || !config_.filter.empty()) return;

    auto currentFiles = discoverRelevantFiles();

    nlohmann::json cache;
    cache["version"] = CHECK_CACHE_VERSION;
    // Mode discriminator: keep L1 and L2 (--deep) verdicts in separate cache
    // entries so a mode switch is treated as a cache miss (see tryCacheHit).
    cache["deep"] = config_.deepMode;

    nlohmann::json filesObj = nlohmann::json::object();
    for (const auto& path : currentFiles) {
        filesObj[path] = hashFile(path);
    }
    cache["files"] = std::move(filesObj);
    cache["result"] = result;

    fs::path cachePath = fs::path(config_.projectDir) / ".topo-check-cache";
    std::ofstream ofs(cachePath);
    if (ofs) {
        ofs << cache.dump(2) << "\n";
    }
}

// ---------------------------------------------------------------------------
// listUnsafePoints — enumerate all detected unsafe points
// ---------------------------------------------------------------------------

int CheckRunner::listUnsafePoints() {
    auto sourceFiles = collectSourceFiles();
    if (sourceFiles.empty()) {
        std::cout << "No source files found.\n";
        return 0;
    }

    // Collect all call sites with unsafe levels (per-file parallel)
    std::vector<check::DetectedCallSite> allSites;
    {
        auto cse = provider_->createCallSiteExtractor();
        if (!cse) {
            std::cerr << "error: call site extraction unavailable\n";
            return -1;
        }
        auto perFile = parallelMap<std::vector<check::DetectedCallSite>>(
            sourceFiles, config_.jobs,
            [&](size_t, const std::string& f) { return cse->extractCallSites(f); });
        for (auto& v : perFile) {
            allSites.insert(allSites.end(), std::make_move_iterator(v.begin()),
                            std::make_move_iterator(v.end()));
        }
    }

    // Collect all imports with unsafe levels (per-file parallel)
    std::vector<check::HostImport> allImports;
    {
        auto ie = provider_->createImportExtractor();
        if (!ie) {
            std::cerr << "error: import extraction unavailable\n";
            return -1;
        }
        auto perFile = parallelMap<std::vector<check::HostImport>>(
            sourceFiles, config_.jobs,
            [&](size_t, const std::string& f) { return ie->extractImports(f); });
        for (auto& v : perFile) {
            allImports.insert(allImports.end(), std::make_move_iterator(v.begin()),
                              std::make_move_iterator(v.end()));
        }
    }

    // Build external function set from SymbolTable
    if (!frontendDone_) {
        if (!runFrontend()) {
            std::cerr << "error: frontend pipeline failed\n";
            return -1;
        }
    }

    std::set<std::string> externalFunctions;
    for (const auto& [name, fn] : symbols_.functions()) {
        if (fn.isExternal) {
            externalFunctions.insert(fn.qualifiedName);
        }
    }

    // Filter and sort: only non-safe points
    int count = 0;

    if (config_.jsonOutput) {
        nlohmann::json arr = nlohmann::json::array();

        for (const auto& site : allSites) {
            if (site.unsafeLevel == check::UnsafeLevel::Safe) continue;
            bool isExt = externalFunctions.count(site.callerQualifiedName) > 0;
            nlohmann::json obj;
            obj["type"] = "call";
            obj["function"] = site.callerQualifiedName;
            obj["pattern"] = site.calleePattern;
            obj["level"] = check::unsafeLevelValue(site.unsafeLevel);
            obj["levelName"] = check::unsafeLevelName(site.unsafeLevel);
            obj["file"] = site.file;
            obj["line"] = site.line;
            obj["external"] = isExt;
            arr.push_back(std::move(obj));
            ++count;
        }

        for (const auto& imp : allImports) {
            if (imp.unsafeLevel == check::UnsafeLevel::Safe) continue;
            nlohmann::json obj;
            obj["type"] = "import";
            obj["pattern"] = imp.normalizedPath;
            obj["level"] = check::unsafeLevelValue(imp.unsafeLevel);
            obj["levelName"] = check::unsafeLevelName(imp.unsafeLevel);
            obj["file"] = imp.file;
            obj["line"] = imp.line;
            arr.push_back(std::move(obj));
            ++count;
        }

        nlohmann::json output;
        output["unsafePoints"] = std::move(arr);
        output["count"] = count;
        std::cout << output.dump(2) << "\n";
    } else {
        // Sort by level descending (highest risk first)
        struct UnsafePoint {
            std::string type;       // "call" or "import"
            std::string function;
            std::string pattern;
            check::UnsafeLevel level;
            std::string file;
            int line;
            bool isExternal;
        };

        std::vector<UnsafePoint> points;

        for (const auto& site : allSites) {
            if (site.unsafeLevel == check::UnsafeLevel::Safe) continue;
            bool isExt = externalFunctions.count(site.callerQualifiedName) > 0;
            points.push_back({"call", site.callerQualifiedName, site.calleePattern,
                              site.unsafeLevel, site.file, site.line, isExt});
        }

        for (const auto& imp : allImports) {
            if (imp.unsafeLevel == check::UnsafeLevel::Safe) continue;
            points.push_back({"import", "", imp.normalizedPath,
                              imp.unsafeLevel, imp.file, imp.line, false});
        }

        // Sort: highest level first, then by file/line
        std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
            if (a.level != b.level) return a.level > b.level;
            if (a.file != b.file) return a.file < b.file;
            return a.line < b.line;
        });

        count = static_cast<int>(points.size());

        std::cout << "Unsafe points: " << count << "\n\n";
        for (const auto& p : points) {
            std::cout << "  [" << check::unsafeLevelName(p.level) << "] ";
            if (p.type == "call") {
                std::cout << p.function;
                if (p.isExternal) std::cout << " (external)";
                std::cout << " -> " << p.pattern;
            } else {
                std::cout << "import " << p.pattern;
            }
            std::cout << " [" << p.file << ":" << p.line << "]\n";
        }
    }

    return count;
}

// ---------------------------------------------------------------------------
// loadConfig — read Topo.toml
// ---------------------------------------------------------------------------

bool CheckRunner::loadConfig() {
    fs::path tomlPath = fs::path(config_.projectDir) / "Topo.toml";
    if (!fs::exists(tomlPath)) {
        std::cerr << "error: Topo.toml not found in " << config_.projectDir << "\n";
        return false;
    }

    toml::parse_result tomlResult = toml::parse_file(tomlPath.string());
    if (!tomlResult) {
        std::cerr << "error: failed to parse Topo.toml: " << tomlResult.error() << "\n";
        return false;
    }

    const auto& tbl = tomlResult.table();

    // [topo].root
    if (auto v = tbl.at_path("topo.root").value<std::string>()) {
        topoRoot_ = (fs::path(config_.projectDir) / *v).string();
    } else {
        std::cerr << "error: [topo].root not found in Topo.toml\n";
        return false;
    }

    // [build].language
    if (auto v = tbl.at_path("build.language").value<std::string>()) {
        language_ = parseHostLanguage(*v);
    }

    // [build].sources + [build].include -> includeDirs_
    auto addDirs = [&](const char* path) {
        if (auto arr = tbl.at_path(path).as_array()) {
            for (const auto& elem : *arr) {
                if (auto sv = elem.value<std::string>()) {
                    includeDirs_.push_back(
                        (fs::path(config_.projectDir) / *sv).string());
                }
            }
        } else if (auto val = tbl.at_path(path).value<std::string>()) {
            includeDirs_.push_back(
                (fs::path(config_.projectDir) / *val).string());
        }
    };
    addDirs("build.sources");
    addDirs("build.include");

    // Mixed projects describe their C++ source set in [build.cpp] (and the
    // rust half via [build.rust].manifest) rather than [build].sources.
    // Resolve the cpp paths into includeDirs_ so the composite provider's
    // cpp half scans them; absent keys degrade to the providers' default
    // project-directory scans (lenient by design).
    std::string rustCrateDir;
    if (language_ == HostLanguage::Mixed) {
        if (auto* srcs = tbl.at_path("build.cpp.sources").as_array()) {
            for (const auto& elem : *srcs) {
                if (auto pat = elem.value<std::string>()) {
                    auto expanded = platform::globExpand(fs::path(config_.projectDir), *pat);
                    includeDirs_.insert(includeDirs_.end(), expanded.begin(), expanded.end());
                }
            }
        }
        if (auto* incs = tbl.at_path("build.cpp.include").as_array()) {
            for (const auto& elem : *incs) {
                if (auto dir = elem.value<std::string>()) {
                    includeDirs_.push_back((fs::path(config_.projectDir) / *dir).string());
                }
            }
        }
        if (auto m = tbl.at_path("build.rust.manifest").value<std::string>()) {
            rustCrateDir = (fs::path(config_.projectDir) / *m).parent_path().string();
        }
    }

    // [completeness] config
    if (auto ct = tbl["completeness"].as_table()) {
        completenessCfg_.ignoreConstructors =
            ct->get("ignore_constructors") ? ct->get("ignore_constructors")->value_or(false) : false;
        completenessCfg_.ignoreDestructors =
            ct->get("ignore_destructors") ? ct->get("ignore_destructors")->value_or(false) : false;
        completenessCfg_.ignoreMain =
            ct->get("ignore_main") ? ct->get("ignore_main")->value_or(true) : true;
        if (auto arr = ct->get("ignore_patterns")) {
            if (auto* a = arr->as_array()) {
                for (const auto& elem : *a) {
                    if (auto sv = elem.value<std::string>()) {
                        completenessCfg_.ignorePatterns.push_back(*sv);
                    }
                }
            }
        }
    }

    // [containment].mode
    if (auto v = tbl.at_path("containment.mode").value<std::string>()) {
        containmentCfg_.mode = parseFeatureMode(*v);
    }

    // [containment].depth — "l2" or "deep" enables L2 LSP analysis
    if (auto v = tbl.at_path("containment.depth").value<std::string>()) {
        if (*v == "l2" || *v == "deep") {
            config_.deepMode = true;
        }
    }

    // [purity].mode
    if (auto v = tbl.at_path("purity.mode").value<std::string>()) {
        purityCfg_.mode = parseFeatureMode(*v);
    }

    // [visibility].mode
    if (auto v = tbl.at_path("visibility.mode").value<std::string>()) {
        visibilityCfg_.mode = parseFeatureMode(*v);
    }

    // [stage_isolation].mode
    if (auto v = tbl.at_path("stage_isolation.mode").value<std::string>()) {
        stageIsolationCfg_.mode = parseFeatureMode(*v);
    }

    // [check].jobs — CLI takes precedence over TOML
    if (!config_.jobsExplicit) {
        if (auto v = tbl.at_path("check.jobs").value<int64_t>()) {
            int tomlJobs = static_cast<int>(*v);
            if (tomlJobs < 0) tomlJobs = 0;
            config_.jobs = tomlJobs;
        }
    }

    // Create language-specific analysis provider via plugin registry.
    // Mixed has no plugin of its own — it composes the cpp and rust
    // providers, so both plugins must be linked in; name the missing
    // one(s) precisely (default-scope installs may carry a subset).
    if (language_ == HostLanguage::Mixed) {
        auto* cppPlugin = lang::getPlugin(HostLanguage::Cpp);
        auto* rustPlugin = lang::getPlugin(HostLanguage::Rust);
        if (!cppPlugin || !rustPlugin) {
            std::cerr << "Error: mixed check requires the cpp and rust language plugins; missing:";
            if (!cppPlugin) std::cerr << " cpp";
            if (!rustPlugin) std::cerr << " rust";
            std::cerr << "\n";
            return false;
        }
        provider_ = std::make_unique<check::MixedAnalysisProvider>(
            cppPlugin->createAnalysisProvider(), rustPlugin->createAnalysisProvider(),
            std::move(rustCrateDir));
    } else {
        auto* plugin = lang::getPlugin(language_);
        if (!plugin) {
            std::cerr << "Error: no language plugin registered for this language\n";
            return false;
        }
        provider_ = plugin->createAnalysisProvider();
    }

    if (config_.verbose) {
        const char* langStr = language_ == HostLanguage::Rust   ? "rust"
                              : language_ == HostLanguage::Java ? "java"
                              : language_ == HostLanguage::Python ? "python"
                              : language_ == HostLanguage::TypeScript ? "typescript"
                              : language_ == HostLanguage::Mixed ? "mixed"
                                                                  : "cpp";
        std::cerr << "topo-check: project=" << config_.projectDir
                  << " language=" << langStr
                  << " root=" << topoRoot_ << "\n";
    }

    return true;
}

// ---------------------------------------------------------------------------
// runFrontend — parse .topo files, build SymbolTable + VisibilityEntries
// ---------------------------------------------------------------------------

bool CheckRunner::runFrontend() {
    if (frontendDone_) return true;

    // Step 1: ImportResolver — DFS resolve, topological order
    DiagnosticEngine diag;
    ImportResolver resolver(diag);
    auto modules = resolver.resolve({topoRoot_});

    if (diag.hasErrors()) {
        diag.print(std::cerr);
        return false;
    }

    if (modules.empty()) {
        std::cerr << "error: no .topo modules resolved from " << topoRoot_ << "\n";
        return false;
    }

    if (config_.verbose) {
        std::cerr << "Resolved " << modules.size() << " .topo module(s)\n";
    }

    // Step 2: Per-module semantic analysis with import context
    std::unordered_map<std::string, SymbolTable> symbolCache;

    for (const auto& mod : modules) {
        SymbolTable importedSymbols;
        for (const auto& directive : mod.imports) {
            auto it = symbolCache.find(directive.resolvedPath);
            if (it != symbolCache.end()) {
                importedSymbols.mergeSelected(it->second, directive.selectedSymbols);
            }
        }

        DiagnosticEngine fileDiag;
        SemanticAnalyzer sema(fileDiag);
        auto fileSymbols = sema.analyze(static_cast<const TopoFile&>(*mod.ast), importedSymbols);

        if (fileDiag.hasErrors()) {
            std::cerr << "error: semantic analysis failed for " << mod.path << "\n";
            fileDiag.print(std::cerr);
            return false;
        }

        symbolCache[mod.path] = std::move(fileSymbols);
    }

    // Step 3: Build combined symbol table
    symbols_ = SymbolTable{};
    for (const auto& mod : modules) {
        auto it = symbolCache.find(mod.path);
        if (it != symbolCache.end()) {
            symbols_.mergeFrom(it->second, /*filterInternal=*/true);
        }
    }

    // Step 4: Collect VisibilityEntries
    VisibilityCollector collector;
    for (const auto& mod : modules) {
        auto entries = collector.collect(static_cast<const TopoFile&>(*mod.ast));
        visEntries_.insert(visEntries_.end(), entries.begin(), entries.end());
    }

    frontendDone_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// collectSourceFiles — scan project for host language source files
// ---------------------------------------------------------------------------

std::vector<std::string> CheckRunner::collectSourceFiles() const {
    return provider_->collectSourceFiles(config_.projectDir, includeDirs_);
}

// ---------------------------------------------------------------------------
// Individual check runners
// ---------------------------------------------------------------------------

check::CheckResult CheckRunner::runCompleteness() {
    check::CheckResult result;

    auto sourceFiles = collectSourceFiles();
    if (sourceFiles.empty()) {
        check::CheckDiagnostic d;
        d.severity = check::Severity::Warning;
        d.check = "completeness";
        d.message = "no source files found";
        result.addDiagnostic(std::move(d));
        return result;
    }

    if (config_.verbose) {
        std::cerr << "Checking completeness: " << sourceFiles.size() << " source file(s)\n";
    }

    auto extractor = provider_->createSymbolExtractor();

    if (!extractor) {
        check::CheckDiagnostic d;
        d.severity = check::Severity::Error;
        d.check = "completeness";
        d.message = "symbol extraction unavailable for this language — cannot verify completeness (is the language server installed?)";
        result.addDiagnostic(std::move(d));
        return result;
    }

    // L2-capable symbol extractors back onto an LSP bridge — serialize (jobs=1)
    // to avoid reentrant bridge calls. L1 regex extractors are safe to parallelize.
    int symbolJobs = provider_->isLSPReady() ? 1 : config_.jobs;
    std::vector<check::HostSymbol> hostSymbols;
    {
        auto perFile = parallelMap<std::vector<check::HostSymbol>>(
            sourceFiles, symbolJobs,
            [&](size_t, const std::string& f) { return extractor->extractSymbols(f); });
        for (auto& v : perFile) {
            hostSymbols.insert(hostSymbols.end(),
                               std::make_move_iterator(v.begin()),
                               std::make_move_iterator(v.end()));
        }
    }

    // L2 extraction may return nothing when the LSP server cannot index the
    // project (e.g., missing compile_commands.json or Java project metadata).
    // Fall back to L1 regex extraction rather than treating it as failure.
    if (hostSymbols.empty() && !sourceFiles.empty() && provider_->isLSPReady()) {
        if (config_.verbose) {
            std::cerr << "topo-check: L2 extraction returned zero symbols, retrying with L1\n";
        }
        provider_->shutdownLSP();
        auto l1Extractor = provider_->createSymbolExtractor();
        if (l1Extractor) {
            auto perFile = parallelMap<std::vector<check::HostSymbol>>(
                sourceFiles, config_.jobs,
                [&](size_t, const std::string& f) { return l1Extractor->extractSymbols(f); });
            for (auto& v : perFile) {
                hostSymbols.insert(hostSymbols.end(),
                                   std::make_move_iterator(v.begin()),
                                   std::make_move_iterator(v.end()));
            }
        }
    }

    if (hostSymbols.empty() && !sourceFiles.empty()) {
        check::CheckDiagnostic d;
        d.severity = check::Severity::Error;
        d.check = "completeness";
        d.message = "extraction returned zero symbols from " + std::to_string(sourceFiles.size()) +
                    " source file(s) — possible extraction failure";
        result.addDiagnostic(std::move(d));
        return result;
    }

    if (config_.verbose) {
        std::cerr << "Extracted " << hostSymbols.size() << " host symbol(s)\n";
    }

    check::checkCompleteness(hostSymbols, symbols_, visEntries_, completenessCfg_, result);
    return result;
}

check::CheckResult CheckRunner::runContainment() {
    check::CheckResult result;

    if (!containmentCfg_.isEnabled()) {
        if (config_.verbose) {
            std::cerr << "Skipping containment check (mode=off)\n";
        }
        // Always emit a note so users know containment was not evaluated
        check::CheckDiagnostic d;
        d.severity = check::Severity::Note;
        d.check = "containment";
        d.message = "containment check is disabled (mode=off in Topo.toml) — "
                    "set [containment].mode to \"auto\" or \"force\" to enable";
        result.addDiagnostic(std::move(d));
        return result;
    }

    // L2 deep mode: delegate to language-specific provider
    if (config_.deepMode) {
        auto sourceFiles = collectSourceFiles();
        auto deepResult = provider_->runDeepContainment(
            symbols_, sourceFiles, containmentCfg_,
            config_.projectDir, config_.verbose);
        if (deepResult) {
            // Check if this is a real analysis result vs an "LSP unavailable" fallback signal.
            // Infrastructure-only results contain only containment-l2 warnings with zero errors.
            bool isInfraOnly = (deepResult->errorCount == 0);
            if (isInfraOnly) {
                for (const auto& d : deepResult->diagnostics) {
                    if (d.check != "containment-l2") {
                        isInfraOnly = false;
                        break;
                    }
                }
            }
            if (!isInfraOnly) return *deepResult;
            // Infrastructure-only result — fall through to L1.
            // Silent degradation is prohibited (principle 16): surface the
            // fallback unconditionally and preserve every L2 diagnostic so
            // users see why L2 failed even when they did not pass --verbose.
            std::cerr << "topo-check: L2 analysis unavailable, falling back to L1\n";
            for (auto& d : deepResult->diagnostics) {
                result.addDiagnostic(std::move(d));
            }
            // The L1 pass below still runs (its findings stay useful), but a
            // run that was asked for L2 depth must not PASS at a silently
            // shallower grade — the no-silent-degradation principle grades a
            // disabled safety check as Error. This diagnostic forces the
            // final verdict non-zero while keeping every L1 finding visible.
            check::CheckDiagnostic deepUnavailable;
            deepUnavailable.severity = check::Severity::Error;
            deepUnavailable.check = "containment";
            deepUnavailable.message =
                "deep (L2) containment analysis was requested (--deep) but its "
                "infrastructure is unavailable — the result above is L1-grade "
                "only; fix the L2 setup (language server / compile database) or "
                "rerun without --deep";
            result.addDiagnostic(std::move(deepUnavailable));
        }
        // Fall through to L1 if deep analysis not supported or unavailable
    }

    auto sourceFiles = collectSourceFiles();
    if (sourceFiles.empty()) {
        return result;
    }

    if (config_.verbose) {
        std::cerr << "Checking containment: " << sourceFiles.size() << " source file(s)\n";
    }

    // Extract imports and call sites via provider
    auto ie = provider_->createImportExtractor();
    if (!ie) {
        check::CheckDiagnostic d;
        d.severity = check::Severity::Error;
        d.check = "containment";
        d.message = "import extraction unavailable for this language — cannot verify containment (is the language server installed?)";
        result.addDiagnostic(std::move(d));
        return result;
    }

    auto cse = provider_->createCallSiteExtractor();
    if (!cse) {
        check::CheckDiagnostic d;
        d.severity = check::Severity::Error;
        d.check = "containment";
        d.message = "call site extraction unavailable for this language — cannot verify containment (is the language server installed?)";
        result.addDiagnostic(std::move(d));
        return result;
    }

    auto se = provider_->createSymbolExtractor();

    // L2 extractors share an LSP bridge — serialize (jobs=1) to avoid
    // reentrant bridge calls. L1 regex extractors are safe to parallelize.
    int extractJobs = provider_->isLSPReady() ? 1 : config_.jobs;

    std::vector<check::HostImport> imports;
    {
        auto perFile = parallelMap<std::vector<check::HostImport>>(
            sourceFiles, extractJobs,
            [&](size_t, const std::string& f) { return ie->extractImports(f); });
        for (auto& v : perFile) {
            imports.insert(imports.end(), std::make_move_iterator(v.begin()),
                           std::make_move_iterator(v.end()));
        }
    }

    std::vector<check::DetectedCallSite> callSites;
    {
        auto perFile = parallelMap<std::vector<check::DetectedCallSite>>(
            sourceFiles, extractJobs,
            [&](size_t, const std::string& f) { return cse->extractCallSites(f); });
        for (auto& v : perFile) {
            callSites.insert(callSites.end(), std::make_move_iterator(v.begin()),
                             std::make_move_iterator(v.end()));
        }
    }

    // Extract host symbols early — they serve two purposes:
    //   (1) supplement call sites with presence entries (F1.7 cross-reference fix)
    //   (2) act as a "extractor health" signal for the empty-extraction guard.
    std::vector<check::HostSymbol> hostSymbols;
    if (se) {
        auto perFile = parallelMap<std::vector<check::HostSymbol>>(
            sourceFiles, extractJobs,
            [&](size_t, const std::string& f) { return se->extractSymbols(f); });
        for (auto& v : perFile) {
            hostSymbols.insert(hostSymbols.end(),
                               std::make_move_iterator(v.begin()),
                               std::make_move_iterator(v.end()));
        }
    }

    // Guard: if source files exist but ALL three extractors returned nothing,
    // extraction has truly failed and proceeding would silently report 0 violations.
    // This is stricter than the old "imports + callsites empty" check, which
    // misclassified clean files (no #include, no unsafe patterns) as
    // extraction failures.
    if (!sourceFiles.empty() && imports.empty() && callSites.empty() &&
        hostSymbols.empty()) {
        check::CheckDiagnostic d;
        d.severity = check::Severity::Error;
        d.check = "containment";
        d.message = "extraction returned zero imports, zero call sites, and zero host symbols from " +
                    std::to_string(sourceFiles.size()) +
                    " source file(s) — possible extraction failure";
        result.addDiagnostic(std::move(d));
        return result;
    }

    // Supplement call sites with host symbol presence entries.
    // checkContainment's F1.7 fix maps external callers → host files via call sites,
    // but misses external functions that only make safe calls (no escape patterns).
    // Adding presence entries lets F1.7 discover all external → host file mappings.
    for (const auto& sym : hostSymbols) {
        if (sym.kind == check::HostSymbolKind::Function ||
            sym.kind == check::HostSymbolKind::Method ||
            sym.kind == check::HostSymbolKind::StaticMethod) {
            check::DetectedCallSite presence;
            presence.callerQualifiedName = sym.simpleName;
            presence.calleePattern = "";
            presence.unsafeLevel = check::UnsafeLevel::Safe;
            presence.file = sym.file;
            presence.line = sym.line;
            callSites.push_back(std::move(presence));
        }
    }

    check::checkContainment(symbols_, imports, callSites, containmentCfg_, result,
                            provider_->separator());
    return result;
}

check::CheckResult CheckRunner::runImportPath() {
    check::CheckResult result;

    analysis::ImportPathConfig ipCfg;
    ipCfg.projectDir = config_.projectDir;
    ipCfg.searchDirs = includeDirs_;
    ipCfg.language = language_;

    analysis::checkImportPaths(symbols_, ipCfg, result);
    return result;
}

check::CheckResult CheckRunner::runPurity() {
    check::CheckResult result;

    if (!purityCfg_.isEnabled()) {
        if (config_.verbose) {
            std::cerr << "Skipping purity check (mode=off)\n";
        }
        check::CheckDiagnostic d;
        d.severity = check::Severity::Note;
        d.check = "purity";
        d.message = "purity check is disabled (mode=off in Topo.toml) — "
                    "set [purity].mode to \"auto\" or \"force\" to enable";
        result.addDiagnostic(std::move(d));
        return result;
    }

    auto sourceFiles = collectSourceFiles();
    if (sourceFiles.empty()) {
        return result;
    }

    if (config_.verbose) {
        std::cerr << "Checking purity: " << sourceFiles.size() << " source file(s)\n";
    }

    auto sae = provider_->createSymbolAccessExtractor();
    if (!sae) {
        check::CheckDiagnostic d;
        d.severity = check::Severity::Error;
        d.check = "purity";
        d.message = "symbol access extraction unavailable for this language — "
                    "cannot verify purity (no CallGraph-aware extractor registered)";
        result.addDiagnostic(std::move(d));
        return result;
    }

    int extractJobs = provider_->isLSPReady() ? 1 : config_.jobs;
    std::vector<check::SymbolAccess> accesses;
    {
        auto perFile = parallelMap<std::vector<check::SymbolAccess>>(
            sourceFiles, extractJobs,
            [&](size_t, const std::string& f) { return sae->extractSymbolAccesses(f); });
        for (auto& v : perFile) {
            accesses.insert(accesses.end(), std::make_move_iterator(v.begin()),
                            std::make_move_iterator(v.end()));
        }
    }

    check::checkPurity(symbols_, accesses, result);
    return result;
}

check::CheckResult CheckRunner::runVisibility() {
    check::CheckResult result;

    if (!visibilityCfg_.isEnabled()) {
        if (config_.verbose) {
            std::cerr << "Skipping visibility check (mode=off)\n";
        }
        check::CheckDiagnostic d;
        d.severity = check::Severity::Note;
        d.check = "visibility";
        d.message = "visibility check is disabled (mode=off in Topo.toml) — "
                    "set [visibility].mode to \"auto\" or \"force\" to enable";
        result.addDiagnostic(std::move(d));
        return result;
    }

    auto sourceFiles = collectSourceFiles();
    if (sourceFiles.empty()) {
        return result;
    }

    if (config_.verbose) {
        std::cerr << "Checking visibility: " << sourceFiles.size() << " source file(s)\n";
    }

    auto cee = provider_->createCallEdgeExtractor();
    if (!cee) {
        check::CheckDiagnostic d;
        d.severity = check::Severity::Error;
        d.check = "visibility";
        d.message = "call edge extraction unavailable for this language — "
                    "cannot verify visibility (no CallGraph-aware extractor registered)";
        result.addDiagnostic(std::move(d));
        return result;
    }

    int extractJobs = provider_->isLSPReady() ? 1 : config_.jobs;
    std::vector<check::CallEdge> edges;
    {
        auto perFile = parallelMap<std::vector<check::CallEdge>>(
            sourceFiles, extractJobs,
            [&](size_t, const std::string& f) { return cee->extractCallEdges(f); });
        for (auto& v : perFile) {
            edges.insert(edges.end(), std::make_move_iterator(v.begin()),
                         std::make_move_iterator(v.end()));
        }
    }

    check::checkVisibilityConsistency(symbols_, visEntries_, edges, result);
    return result;
}

check::CheckResult CheckRunner::runStageIsolation() {
    check::CheckResult result;

    if (!stageIsolationCfg_.isEnabled()) {
        if (config_.verbose) {
            std::cerr << "Skipping stage-isolation check (mode=off)\n";
        }
        check::CheckDiagnostic d;
        d.severity = check::Severity::Note;
        d.check = "stage-isolation";
        d.message = "stage-isolation check is disabled (mode=off in Topo.toml) — "
                    "set [stage_isolation].mode to \"auto\" or \"force\" to enable";
        result.addDiagnostic(std::move(d));
        return result;
    }

    auto sourceFiles = collectSourceFiles();
    if (sourceFiles.empty()) {
        return result;
    }

    if (config_.verbose) {
        std::cerr << "Checking stage isolation: " << sourceFiles.size() << " source file(s)\n";
    }

    auto cee = provider_->createCallEdgeExtractor();
    if (!cee) {
        check::CheckDiagnostic d;
        d.severity = check::Severity::Error;
        d.check = "stage-isolation";
        d.message = "call edge extraction unavailable for this language — "
                    "cannot verify stage isolation (no CallGraph-aware extractor registered)";
        result.addDiagnostic(std::move(d));
        return result;
    }

    int extractJobs = provider_->isLSPReady() ? 1 : config_.jobs;
    std::vector<check::CallEdge> edges;
    {
        auto perFile = parallelMap<std::vector<check::CallEdge>>(
            sourceFiles, extractJobs,
            [&](size_t, const std::string& f) { return cee->extractCallEdges(f); });
        for (auto& v : perFile) {
            edges.insert(edges.end(), std::make_move_iterator(v.begin()),
                         std::make_move_iterator(v.end()));
        }
    }

    check::checkStageIsolation(symbols_, edges, result);
    return result;
}

// ---------------------------------------------------------------------------
// Filter + reporting helpers
// ---------------------------------------------------------------------------

bool CheckRunner::matchesFilter(const std::string& checkName) const {
    if (config_.filter.empty()) return true;
    return checkName.find(config_.filter) != std::string::npos;
}

static const char* severityStr(check::Severity s) {
    switch (s) {
    case check::Severity::Error: return "ERROR";
    case check::Severity::Warning: return "WARNING";
    case check::Severity::Note: return "NOTE";
    }
    return "UNKNOWN";
}

void CheckRunner::reportResult(const std::string& checkName, const check::CheckResult& result) {
    if (result.diagnostics.empty()) {
        std::cout << "[" << checkName << "] OK\n";
        return;
    }

    std::cout << "[" << checkName << "] " << result.errorCount << " error(s), "
              << result.warningCount << " warning(s):\n";

    // Deterministic output under parallel extraction — sort by (file, line, column, check).
    std::vector<const check::CheckDiagnostic*> sorted;
    sorted.reserve(result.diagnostics.size());
    for (const auto& d : result.diagnostics) sorted.push_back(&d);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const check::CheckDiagnostic* a, const check::CheckDiagnostic* b) {
                         if (a->file != b->file) return a->file < b->file;
                         if (a->line != b->line) return a->line < b->line;
                         if (a->column != b->column) return a->column < b->column;
                         return a->check < b->check;
                     });

    for (const auto* d : sorted) {
        std::cout << "  " << severityStr(d->severity) << ": " << d->message;
        if (!d->file.empty()) {
            std::cout << " [" << d->file;
            if (d->line > 0) std::cout << ":" << d->line;
            std::cout << "]";
        }
        std::cout << "\n";
    }
}

void CheckRunner::reportResultJson(const std::string& checkName, const check::CheckResult& result) {
    // JSON output is assembled in run() using nlohmann::json
    // This method is kept for potential per-check streaming in the future.
    (void)checkName;
    (void)result;
}

// ---------------------------------------------------------------------------
// run — orchestrate all checks
// ---------------------------------------------------------------------------

int CheckRunner::run() {
    // Incremental: check cache before expensive work
    int cachedResult = -1;
    if (tryCacheHit(cachedResult)) {
        if (config_.verbose) {
            std::cerr << "topo-check: cache hit, skipping checks\n";
        }
        if (config_.jsonOutput) {
            // JSON output: emit a minimal verdict object so stdout-parsing
            // consumers see `passed` without re-running the checks.
            nlohmann::json output;
            output["project"] = config_.projectDir;
            output["passed"] = (cachedResult == 0);
            output["cached"] = true;
            std::cout << output.dump(2) << "\n";
        } else {
            // Textual output: include the same "Result: PASS"/"Result: FAIL"
            // verdict line the full path emits, so stdout consumers can rely
            // on a uniform contract regardless of whether the cache hit.
            std::cout << "topo-check: cache hit (no files changed)\n";
            std::cout << "\nResult: " << (cachedResult == 0 ? "PASS" : "FAIL")
                      << " (cached)\n";
        }
        return cachedResult;
    }

    if (!runFrontend()) return 2;

    if (config_.verbose) {
        std::cerr << "topo-check: jobs=" << resolveJobs(config_.jobs) << "\n";
    }

    // Initialize LSP only when deep (L2) analysis is requested.
    // LSP servers like jdtls have heavy startup cost (~10s JVM) that
    // should not penalize L1-only checks.
    if (provider_ && config_.deepMode) {
        if (provider_->initLSP(config_.projectDir, config_.verbose)) {
            if (config_.verbose) {
                std::cerr << "topo-check: LSP initialized\n";
            }
        } else {
            // --deep promised L2 depth and the language server is the L2
            // engine. Substituting regex extractors here would let a PASS
            // mean something strictly shallower than what was requested —
            // the no-silent-degradation principle grades a disabled safety
            // check as Error: abort, non-zero. Users who want the L1-grade
            // result can rerun without --deep.
            std::cerr << "topo-check: error: --deep requested but the language server "
                         "is unavailable — deep (L2) analysis cannot run.\n"
                         "  Install/start the language server for this project's "
                         "language, or rerun without --deep for L1-only checks.\n";
            return 2;
        }
    }

    bool anyErrors = false;
    lastResults_.clear();
    std::vector<std::pair<std::string, check::CheckResult>> results;

    auto shouldRun = [&](const std::string& name) {
        if (!matchesFilter(name)) return false;
        return config_.checkName == "all" || config_.checkName == name;
    };

    if (shouldRun("completeness")) {
        auto r = runCompleteness();
        if (!r.passed()) anyErrors = true;
        results.push_back({"completeness", std::move(r)});
    }
    if (shouldRun("containment")) {
        auto r = runContainment();
        if (!r.passed()) anyErrors = true;
        results.push_back({"containment", std::move(r)});
    }
    if (shouldRun("import-path")) {
        auto r = runImportPath();
        if (!r.passed()) anyErrors = true;
        results.push_back({"import-path", std::move(r)});
    }
    if (shouldRun("purity")) {
        auto r = runPurity();
        if (!r.passed()) anyErrors = true;
        results.push_back({"purity", std::move(r)});
    }
    if (shouldRun("visibility")) {
        auto r = runVisibility();
        if (!r.passed()) anyErrors = true;
        results.push_back({"visibility", std::move(r)});
    }
    if (shouldRun("stage-isolation")) {
        auto r = runStageIsolation();
        if (!r.passed()) anyErrors = true;
        results.push_back({"stage-isolation", std::move(r)});
    }

    // Store results for programmatic access
    lastResults_ = results;

    // Report
    if (config_.jsonOutput) {
        const char* langStr = language_ == HostLanguage::Rust   ? "rust"
                              : language_ == HostLanguage::Java ? "java"
                              : language_ == HostLanguage::Python ? "python"
                              : language_ == HostLanguage::TypeScript ? "typescript"
                              : language_ == HostLanguage::Mixed ? "mixed"
                                                                  : "cpp";

        nlohmann::json output;
        output["project"] = config_.projectDir;
        output["language"] = langStr;
        output["passed"] = !anyErrors;

        nlohmann::json checksArr = nlohmann::json::array();
        for (const auto& [name, res] : results) {
            nlohmann::json checkObj;
            checkObj["name"] = name;
            checkObj["passed"] = res.passed();
            checkObj["errorCount"] = res.errorCount;
            checkObj["warningCount"] = res.warningCount;
            checkObj["truncated"] = res.truncated;

            nlohmann::json diagArr = nlohmann::json::array();
            for (const auto& d : res.diagnostics) {
                nlohmann::json dObj;
                dObj["severity"] = (d.severity == check::Severity::Error)     ? "error"
                                   : (d.severity == check::Severity::Warning) ? "warning"
                                                                              : "note";
                dObj["check"] = d.check.empty() ? name : d.check;
                dObj["message"] = d.message;
                if (!d.file.empty()) dObj["file"] = d.file;
                if (d.line > 0) dObj["line"] = d.line;
                diagArr.push_back(std::move(dObj));
            }
            checkObj["diagnostics"] = std::move(diagArr);
            checksArr.push_back(std::move(checkObj));
        }
        output["checks"] = std::move(checksArr);

        std::cout << output.dump(2) << "\n";
    } else {
        const char* langStr = language_ == HostLanguage::Rust   ? "Rust"
                              : language_ == HostLanguage::Java ? "Java"
                              : language_ == HostLanguage::Python ? "Python"
                              : language_ == HostLanguage::TypeScript ? "TypeScript"
                              : language_ == HostLanguage::Mixed ? "Mixed (C++/Rust)"
                                                                  : "C++";
        std::cout << "topo-check: " << config_.projectDir << " (" << langStr << ")\n\n";

        for (const auto& [name, res] : results) {
            reportResult(name, res);
        }

        for (const auto& [name, res] : results) {
            if (res.truncated) {
                std::cout << "  NOTE: [" << name << "] diagnostics truncated at "
                          << check::kMaxCheckDiagnostics << " — "
                          << res.errorCount << " total error(s), "
                          << res.warningCount << " total warning(s)\n";
            }
        }

        // Containment shallow-analysis disclosure (issues #4/#10/#13 design-limit).
        // Show whenever containment ran AND either had violations or verbose mode is on.
        // Prevents spam in clean builds while ensuring users acting on a violation
        // see what containment is *not* analyzing.
        for (const auto& [name, res] : results) {
            if (name != "containment") continue;
            bool hasViolation = res.errorCount > 0 || res.warningCount > 0;
            if (hasViolation || config_.verbose) {
                std::cout << "  NOTE: containment is a shallow analysis — direct calls only; "
                             "transitive paths, cast-laundered values, and macro/proc-macro "
                             "expansions are not analyzed. "
                             "See the containment-analysis limitations note.\n";
            }
            break;
        }

        int totalErrors = 0, totalWarnings = 0;
        for (const auto& [name, res] : results) {
            totalErrors += res.errorCount;
            totalWarnings += res.warningCount;
        }

        std::cout << "\nResult: " << (anyErrors ? "FAIL" : "PASS");
        if (totalErrors > 0 || totalWarnings > 0) {
            std::cout << " (" << totalErrors << " error(s), " << totalWarnings << " warning(s))";
        }
        std::cout << "\n";
    }

    // Shut down LSP bridge
    if (provider_) provider_->shutdownLSP();

    int result = anyErrors ? 1 : 0;
    saveCheckCache(result);
    return result;
}

} // namespace topo
