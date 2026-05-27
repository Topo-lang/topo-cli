#ifndef TOPO_BUILD_CONFIG_H
#define TOPO_BUILD_CONFIG_H

#include "topo/Build/BuildConfig.h"

#include <string>

namespace topo::build {

/// Print usage information.
void printUsage(const char* argv0);

/// Parse command-line arguments into BuildConfig.
bool parseArgs(int argc, char* argv[], BuildConfig& cfg);

/// Load Topo.toml from the current directory and populate cfg.
bool loadTopoToml(BuildConfig& cfg);

/// Ensure outputPath has the correct extension for the target output type.
void autoExtension(std::string& outputPath, OutputType outputType);

} // namespace topo::build

#endif // TOPO_BUILD_CONFIG_H
