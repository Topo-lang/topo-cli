#include "InitConfig.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>

namespace fs = std::filesystem;

namespace topo::init {

void printUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [options]\n"
              << "\n"
              << "  --project <dir>      Project directory (default: current directory)\n"
              << "  --language <lang>    Language: cpp, rust, java, python, typescript (default: auto-detect)\n"
              << "  --output <dir>       Output directory for .topo files (default: topo/)\n"
              << "  --dry-run            Print generated .topo to stdout, don't write files\n"
              << "  --verbose            Print extracted symbols\n"
              << "  --help               Show this help\n";
}

bool parseArgs(int argc, char* argv[], InitConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--project") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --project requires an argument\n";
                return false;
            }
            cfg.projectDir = argv[++i];
        } else if (std::strcmp(argv[i], "--language") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --language requires an argument\n";
                return false;
            }
            cfg.language = topo::parseHostLanguage(argv[++i]);
            cfg.autoDetectLanguage = false;
        } else if (std::strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --output requires an argument\n";
                return false;
            }
            cfg.outputDir = argv[++i];
        } else if (std::strcmp(argv[i], "--dry-run") == 0) {
            cfg.dryRun = true;
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            cfg.verbose = true;
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

/// Directories to skip during recursive scanning.
static bool shouldSkipDir(const std::string& name) {
    return name == "build" || name == ".git" || name == "llvm-dev" || name == "node_modules";
}

HostLanguage detectLanguage(const std::string& projectDir) {
    int cppCount = 0, rustCount = 0, javaCount = 0, pythonCount = 0, typescriptCount = 0;

    std::error_code ec;
    // Use the non-throwing (error_code) overloads of increment and the
    // status queries. skip_permission_denied only suppresses permission
    // errors at construction/advance; a dangling symlink or a transient FS
    // error would otherwise throw filesystem_error out of an uncaught path
    // and terminate the tool. Clear+check ec each step and skip a bad entry
    // rather than crash — detection should degrade gracefully.
    for (auto it = fs::recursive_directory_iterator(projectDir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        bool isDir = it->is_directory(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (isDir) {
            auto dirName = it->path().filename().string();
            if (shouldSkipDir(dirName)) {
                it.disable_recursion_pending();
                continue;
            }
        }
        bool isFile = it->is_regular_file(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (!isFile) continue;

        auto ext = it->path().extension().string();
        if (ext == ".cpp" || ext == ".cc" || ext == ".cxx")
            ++cppCount;
        else if (ext == ".rs")
            ++rustCount;
        else if (ext == ".java")
            ++javaCount;
        else if (ext == ".py")
            ++pythonCount;
        else if (ext == ".ts" || ext == ".tsx")
            ++typescriptCount;
    }

    int maxOther = std::max({cppCount, rustCount, javaCount, pythonCount});
    if (typescriptCount > maxOther) return HostLanguage::TypeScript;
    if (pythonCount > cppCount && pythonCount > rustCount && pythonCount > javaCount) return HostLanguage::Python;
    if (rustCount > cppCount && rustCount > javaCount) return HostLanguage::Rust;
    if (javaCount > cppCount && javaCount > rustCount) return HostLanguage::Java;
    return HostLanguage::Cpp;
}

/// Check if an extension matches the given language.
static bool matchesExtension(const std::string& ext, HostLanguage lang) {
    switch (lang) {
    case HostLanguage::Cpp: return ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".h" || ext == ".hpp";
    case HostLanguage::Rust: return ext == ".rs";
    case HostLanguage::Java: return ext == ".java";
    case HostLanguage::Python: return ext == ".py";
    case HostLanguage::TypeScript: return ext == ".ts" || ext == ".tsx";
    case HostLanguage::Mixed:
        return ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".h" || ext == ".hpp" || ext == ".rs";
    }
    return false;
}

/// Directories to skip during source collection.
static bool shouldSkipCollectDir(const std::string& name) {
    return name == "build" || name == ".git" || name == "llvm-dev" || name == "test";
}

std::vector<std::string> collectSourceFiles(const std::string& projectDir, HostLanguage language) {
    std::set<std::string> files;
    std::error_code ec;

    auto scanRecursive = [&](const fs::path& dir) {
        if (!fs::exists(dir, ec)) return;
        // Non-throwing increment + status queries (see detectLanguage): a
        // dangling symlink or transient FS error must not throw out of the
        // (try/catch-less) caller. Clear+check ec each step; skip on error.
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            bool isDir = it->is_directory(ec);
            if (ec) {
                ec.clear();
                continue;
            }
            if (isDir) {
                auto dirName = it->path().filename().string();
                if (shouldSkipCollectDir(dirName)) {
                    it.disable_recursion_pending();
                    continue;
                }
            }
            bool isFile = it->is_regular_file(ec);
            if (ec) {
                ec.clear();
                continue;
            }
            if (!isFile) continue;
            auto ext = it->path().extension().string();
            if (matchesExtension(ext, language)) {
                // canonical()'s ec-overload returns an EMPTY path on failure
                // (broken symlink, file removed between enumeration and
                // canonicalization). Inserting that empty string would feed a
                // bogus "" path to the symbol extractor — check ec and fall
                // back to the non-canonical path instead.
                auto canon = fs::canonical(it->path(), ec);
                if (ec) {
                    ec.clear();
                    files.insert(it->path().string());
                } else {
                    files.insert(canon.string());
                }
            }
        }
    };

    // Scan the project root non-recursively for source files (top-level
    // sources outside src/). Same ec-safe pattern as scanRecursive: explicit
    // increment(ec) instead of the range-for's hidden throwing operator++,
    // is_regular_file(ec), and a checked canonical() so a transient FS error
    // or broken symlink degrades instead of throwing / inserting "".
    auto scanRootNonRecursive = [&]() {
        auto rootDir = fs::path(projectDir);
        auto it = fs::directory_iterator(rootDir, ec);
        if (ec) {
            ec.clear();
            return;
        }
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            bool isFile = it->is_regular_file(ec);
            if (ec) {
                ec.clear();
                continue;
            }
            if (!isFile) continue;
            auto ext = it->path().extension().string();
            if (matchesExtension(ext, language)) {
                auto canon = fs::canonical(it->path(), ec);
                if (ec) {
                    ec.clear();
                    files.insert(it->path().string());
                } else {
                    files.insert(canon.string());
                }
            }
        }
    };

    if (language == HostLanguage::Cpp) {
        // Scan src/ recursively
        scanRecursive(fs::path(projectDir) / "src");
        // Scan project root non-recursively
        scanRootNonRecursive();
    } else if (language == HostLanguage::Rust) {
        scanRecursive(fs::path(projectDir) / "src");
    } else if (language == HostLanguage::Java) {
        auto mavenDir = fs::path(projectDir) / "src" / "main" / "java";
        if (fs::exists(mavenDir, ec)) {
            scanRecursive(mavenDir);
        } else {
            scanRecursive(fs::path(projectDir) / "src");
        }
    } else if (language == HostLanguage::Python || language == HostLanguage::TypeScript) {
        // Python/TypeScript projects: scan src/ recursively plus top-level
        // sources, mirroring the Cpp branch. Without these branches
        // collectSourceFiles returned an empty vector and `topo init` aborted
        // with "no source files found" — even though detectLanguage and
        // matchesExtension already recognise .py/.ts/.tsx.
        scanRecursive(fs::path(projectDir) / "src");
        scanRootNonRecursive();
    }

    return std::vector<std::string>(files.begin(), files.end());
}

} // namespace topo::init
