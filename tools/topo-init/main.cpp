#include "InitConfig.h"
#include "TopoGenerator.h"
#include "topo/Check/LanguageAnalysisProvider.h"
#include "topo/Check/SymbolExtractor.h"
#include "topo/Lang/LanguagePlugin.h"

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
#include <fstream>
#include <iostream>
#include <memory>

namespace fs = std::filesystem;

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

    topo::init::InitConfig cfg;
    if (!topo::init::parseArgs(argc, argv, cfg)) return 1;

    // Auto-detect language if not specified
    if (cfg.autoDetectLanguage) {
        cfg.language = topo::init::detectLanguage(cfg.projectDir);
        if (cfg.verbose) {
            // Map every detectable language; previously Python/TypeScript fell
            // through to the literal "cpp", so a detected Python project
            // printed "Detected language: cpp".
            const char* langStr =
                cfg.language == topo::HostLanguage::Rust         ? "rust"
                : cfg.language == topo::HostLanguage::Java       ? "java"
                : cfg.language == topo::HostLanguage::Python     ? "python"
                : cfg.language == topo::HostLanguage::TypeScript ? "typescript"
                                                                 : "cpp";
            std::cerr << "Detected language: " << langStr << "\n";
        }
    }

    // Collect source files
    auto sourceFiles = topo::init::collectSourceFiles(cfg.projectDir, cfg.language);
    if (sourceFiles.empty()) {
        std::cerr << "Error: no source files found in " << cfg.projectDir << "\n";
        return 1;
    }
    if (cfg.verbose) {
        std::cerr << "Found " << sourceFiles.size() << " source files\n";
        for (const auto& f : sourceFiles)
            std::cerr << "  " << f << "\n";
    }

    // Create extractor and extract symbols
    // (C++ regex extractor; non-C++ languages will need LSP-based extraction)
    if (cfg.language != topo::HostLanguage::Cpp && cfg.verbose) {
        std::cerr << "warning: regex-based symbol extraction is only available for C++; "
                     "non-C++ languages require LSP-based extraction\n";
    }
    std::unique_ptr<topo::check::SymbolExtractor> extractor;
    auto* plugin = topo::lang::getPlugin(cfg.language);
    if (plugin) {
        auto provider = plugin->createAnalysisProvider();
        extractor = provider->createSymbolExtractor();
    }
    if (!extractor) {
        std::cerr << "Error: no symbol extractor available for this language\n";
        return 1;
    }
    auto symbols = extractor->extractAll(sourceFiles);

    if (cfg.verbose) {
        std::cerr << "Extracted " << symbols.size() << " symbols\n";
        for (const auto& s : symbols) {
            std::cerr << "  " << s.qualifiedName;
            if (s.hostVisibility) {
                switch (*s.hostVisibility) {
                case topo::Visibility::Public: std::cerr << " [public]"; break;
                case topo::Visibility::Protected: std::cerr << " [protected]"; break;
                case topo::Visibility::Private: std::cerr << " [private]"; break;
                default: break;
                }
            }
            std::cerr << "\n";
        }
    }

    if (symbols.empty()) {
        std::cerr << "Warning: no symbols extracted\n";
        return 0;
    }

    // Generate .topo and Topo.toml
    std::string projectName = fs::path(fs::absolute(cfg.projectDir)).filename().string();
    topo::init::TopoGenerator gen(cfg.language, projectName);

    // Determine sources glob for Topo.toml. Each language gets its own
    // extension; Python/TypeScript previously fell through to "src/**/*.cpp",
    // writing a glob that matched none of their sources.
    std::string sourcesGlob;
    if (cfg.language == topo::HostLanguage::Rust)
        sourcesGlob = "src/**/*.rs";
    else if (cfg.language == topo::HostLanguage::Java)
        sourcesGlob = "src/**/*.java";
    else if (cfg.language == topo::HostLanguage::Python)
        sourcesGlob = "src/**/*.py";
    else if (cfg.language == topo::HostLanguage::TypeScript)
        sourcesGlob = "src/**/*.ts";
    else
        sourcesGlob = "src/**/*.cpp";

    auto result = gen.generate(symbols, sourcesGlob);

    if (cfg.dryRun) {
        for (const auto& f : result.topoFiles) {
            std::cout << "// === " << f.relativePath << " ===\n";
            std::cout << f.content << "\n";
        }
        std::cout << "// === Topo.toml ===\n";
        std::cout << result.topoToml << "\n";
        return 0;
    }

    // Write files
    auto topoDir = fs::path(cfg.projectDir) / cfg.outputDir;
    fs::create_directories(topoDir);

    for (const auto& f : result.topoFiles) {
        auto outPath = fs::path(cfg.projectDir) / f.relativePath;
        fs::create_directories(outPath.parent_path());
        std::ofstream out(outPath);
        out << f.content;
        std::cerr << "Generated: " << f.relativePath << "\n";
    }

    auto tomlPath = fs::path(cfg.projectDir) / "Topo.toml";
    std::ofstream tomlOut(tomlPath);
    tomlOut << result.topoToml;
    std::cerr << "Generated: Topo.toml\n";

    // Lay down the zero-install native-lldb formatter as a courtesy:
    // write the language's lldb_formatter.py into the scaffold plus a
    // project `.lldbinit` that imports it. Providers without a formatter
    // (Java/Python/TypeScript) return empty strings, so this is a clean
    // no-op for them — no stray `.lldbinit` is created.
    if (plugin) {
        if (auto* itp = plugin->initTemplateProvider()) {
            std::string relPath = itp->lldbFormatterRelPath();
            std::string script = itp->lldbFormatterScript();
            if (!relPath.empty() && !script.empty()) {
                std::string lldbInit = itp->generateLldbInit(relPath);
                if (!lldbInit.empty()) {
                    auto fmtPath = fs::path(cfg.projectDir) / relPath;
                    fs::create_directories(fmtPath.parent_path());
                    std::ofstream fmtOut(fmtPath);
                    fmtOut << script;
                    std::cerr << "Generated: " << relPath << "\n";

                    auto lldbInitPath =
                        fs::path(cfg.projectDir) / ".lldbinit";
                    std::ofstream liOut(lldbInitPath);
                    liOut << lldbInit;
                    std::cerr << "Generated: .lldbinit\n";
                }
            }
        }
    }

    std::cerr << "\nDone. " << symbols.size() << " symbols → " << result.topoFiles.size() << " .topo file(s)\n"
              << "Run 'topo-check --check completeness' to verify declaration consistency.\n";
    return 0;
}
