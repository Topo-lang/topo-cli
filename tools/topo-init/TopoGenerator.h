#ifndef TOPO_INIT_TOPOGENERATOR_H
#define TOPO_INIT_TOPOGENERATOR_H

#include "topo/Check/SymbolExtractor.h"
#include "topo/Basic/HostLanguage.h"
#include <string>
#include <vector>

namespace topo::init {

struct GeneratedFile {
    std::string relativePath;
    std::string content;
};

struct GeneratorResult {
    std::vector<GeneratedFile> topoFiles;
    std::string topoToml;
};

class TopoGenerator {
public:
    explicit TopoGenerator(HostLanguage lang, const std::string& projectName);

    GeneratorResult generate(const std::vector<check::HostSymbol>& symbols, const std::string& sourcesGlob) const;

private:
    HostLanguage lang_;
    std::string projectName_;

    std::string generateTypeBindings() const;
    std::string generateTopoBody(const std::vector<check::HostSymbol>& symbols) const;
    std::string generateTopoToml(const std::string& sourcesGlob) const;
};

} // namespace topo::init
#endif
