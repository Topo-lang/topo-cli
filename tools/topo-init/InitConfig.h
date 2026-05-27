#ifndef TOPO_INIT_INITCONFIG_H
#define TOPO_INIT_INITCONFIG_H

#include "topo/Basic/HostLanguage.h"
#include <string>
#include <vector>

namespace topo::init {

struct InitConfig {
    std::string projectDir = ".";
    HostLanguage language = HostLanguage::Cpp;
    bool autoDetectLanguage = true;
    std::string outputDir = "topo";
    bool dryRun = false;
    bool verbose = false;
};

void printUsage(const char* argv0);
bool parseArgs(int argc, char* argv[], InitConfig& cfg);

/// Auto-detect language from file extensions in projectDir.
HostLanguage detectLanguage(const std::string& projectDir);

/// Collect source files for the given language.
/// C++: scans src/ + project root for .cpp/.cc/.cxx/.h/.hpp
/// Rust: scans src/ for .rs
/// Java: scans src/main/java/ then src/ for .java
std::vector<std::string> collectSourceFiles(const std::string& projectDir, HostLanguage language);

} // namespace topo::init
#endif
