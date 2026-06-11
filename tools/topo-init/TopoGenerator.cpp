#include "TopoGenerator.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

using namespace topo::check;

namespace topo::init {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Map a Visibility enum to its .topo section keyword.
const char* visibilitySectionName(Visibility v) {
    switch (v) {
    case Visibility::Public: return "public";
    case Visibility::Protected: return "protected";
    case Visibility::Private: return "private";
    case Visibility::Internal: return "internal";
    case Visibility::Ignore: return "ignore";
    }
    return "protected";
}

/// Effective visibility for a symbol.  If the host didn't supply one we
/// default to Protected and mark it for user review.
Visibility effectiveVisibility(const HostSymbol& sym) {
    return sym.hostVisibility.value_or(Visibility::Protected);
}

bool needsTodoComment(const HostSymbol& sym) {
    return !sym.hostVisibility.has_value();
}

/// Split a qualifiedName into (namespace, localName).
/// "engine::core::Widget::init" with enclosingClass "engine::core::Widget"
///   → namespace "engine::core", class "Widget"
/// "engine::core::init" (free function) → namespace "engine::core", name "init"
/// "init" → namespace "", name "init"
std::pair<std::string, std::string> splitNamespace(const std::string& qualifiedName,
                                                   const std::string& enclosingClass) {
    // For members the namespace is everything before the class name.
    if (!enclosingClass.empty()) {
        auto pos = enclosingClass.rfind("::");
        if (pos != std::string::npos) return {enclosingClass.substr(0, pos), enclosingClass.substr(pos + 2)};
        // Class with no namespace prefix.
        return {"", enclosingClass};
    }
    // Free function / type: namespace is everything before the last "::".
    auto pos = qualifiedName.rfind("::");
    if (pos != std::string::npos) return {qualifiedName.substr(0, pos), qualifiedName.substr(pos + 2)};
    return {"", qualifiedName};
}

std::string extractClassName(const std::string& enclosingClass) {
    auto pos = enclosingClass.rfind("::");
    if (pos != std::string::npos) return enclosingClass.substr(pos + 2);
    return enclosingClass;
}

/// Join parameters with ", ". Host extractors supply types only (HostSymbol
/// carries no parameter names) while the grammar requires
/// Parameter ::= Type Identifier — synthesize argN names.
std::string renderParams(const std::vector<std::string>& paramTypes) {
    std::ostringstream out;
    for (size_t i = 0; i < paramTypes.size(); ++i) {
        if (i > 0) out << ", ";
        out << paramTypes[i] << " arg" << i;
    }
    return out.str();
}

/// Sanitize a project name into a legal .topo namespace identifier:
/// non-alphanumerics map to '_', a leading digit gets a '_' prefix, an
/// empty result falls back to "app". The '-' → '_' mapping matches
/// cargo's package-name convention, keeping the wrapper namespace equal
/// to a Rust crate's symbol-form name (which IR-level symbol mapping
/// resolves against).
std::string sanitizeNamespaceName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_';
        out.push_back(ok ? c : '_');
    }
    if (out.empty()) return "app";
    if (out[0] >= '0' && out[0] <= '9') out.insert(out.begin(), '_');
    return out;
}

/// Return type string: use extracted type if available, else "void".
std::string returnTypeOrVoid(const HostSymbol& sym) {
    if (!sym.returnType.empty()) return sym.returnType;
    return "void";
}

/// Render a single symbol declaration line (without indentation or newline).
std::string renderSymbol(const HostSymbol& sym) {
    std::ostringstream out;
    std::string params = renderParams(sym.paramTypes);
    switch (sym.kind) {
    case HostSymbolKind::Constructor:
        // The grammar names a constructor after its type; host extractors
        // surface language-specific spellings (__init__, constructor), so
        // derive the name from the enclosing class instead.
        out << extractClassName(sym.enclosingClass) << "(" << params << ");";
        break;
    case HostSymbolKind::Destructor: out << "~" << extractClassName(sym.enclosingClass) << "();"; break;
    case HostSymbolKind::StaticMethod:
        out << "static " << returnTypeOrVoid(sym) << " " << sym.simpleName << "(" << params << ");";
        break;
    default: // Function, Method
        out << returnTypeOrVoid(sym) << " " << sym.simpleName << "(" << params << ")";
        if (sym.isConst) out << " const";
        out << ";";
        break;
    }
    if (needsTodoComment(sym)) out << "  // TODO: verify visibility";
    return out.str();
}

// Visibility ordering for output sections.
int visibilityOrder(Visibility v) {
    switch (v) {
    case Visibility::Public: return 0;
    case Visibility::Protected: return 1;
    case Visibility::Private: return 2;
    case Visibility::Internal: return 3;
    case Visibility::Ignore: return 4;
    }
    return 5;
}

// ---- Grouping structures ------------------------------------------------

struct ClassContent {
    // Visibility of the type declaration itself — it places the `type`
    // block inside a namespace visibility section. When the host extractor
    // supplies no type symbol (members only), the Protected default stands
    // and the declaration is flagged for user review.
    Visibility visibility = Visibility::Protected;
    bool hasTodoComment = true; // cleared when the type symbol carries a host visibility
    // Members grouped by visibility.
    std::map<Visibility, std::vector<std::string>> membersByVisibility;
};

struct NamespaceContent {
    // Free functions/symbols grouped by visibility.
    std::map<Visibility, std::vector<std::string>> freeByVisibility;
    // Classes keyed by simple class name, in insertion order via std::map.
    std::map<std::string, ClassContent> classes;
};

/// Write indentation.
void indent(std::ostringstream& out, int level) {
    for (int i = 0; i < level; ++i)
        out << "  ";
}

/// Emit visibility-grouped symbols with the given base indentation level.
void emitVisibilitySections(std::ostringstream& out,
                            const std::map<Visibility, std::vector<std::string>>& groups,
                            int baseIndent) {
    // Sort visibility sections in canonical order.
    std::vector<std::pair<Visibility, const std::vector<std::string>*>> sorted;
    for (auto& [vis, syms] : groups)
        sorted.push_back({vis, &syms});
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) {
        return visibilityOrder(a.first) < visibilityOrder(b.first);
    });

    for (auto& [vis, syms] : sorted) {
        if (syms->empty()) continue;
        indent(out, baseIndent);
        out << visibilitySectionName(vis) << ":\n";
        for (auto& s : *syms) {
            indent(out, baseIndent + 1);
            out << s << "\n";
        }
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// TopoGenerator
// ---------------------------------------------------------------------------

TopoGenerator::TopoGenerator(HostLanguage lang, const std::string& projectName)
    : lang_(lang), projectName_(projectName) {}

GeneratorResult TopoGenerator::generate(const std::vector<HostSymbol>& symbols, const std::string& sourcesGlob) const {
    GeneratorResult result;

    std::string body;
    {
        std::ostringstream out;
        out << "// Auto-generated by topo-init\n";
        out << "// Review and adjust visibility, types, and parameters.\n\n";
        out << generateTypeBindings();
        out << "\n";
        out << generateTopoBody(symbols);
        body = out.str();
    }

    GeneratedFile mainTopo;
    mainTopo.relativePath = "topo/main.topo";
    mainTopo.content = body;
    result.topoFiles.push_back(std::move(mainTopo));
    result.topoToml = generateTopoToml(sourcesGlob);
    return result;
}

std::string TopoGenerator::generateTypeBindings() const {
    std::ostringstream out;
    switch (lang_) {
    case HostLanguage::Cpp:
        out << "using int = std::cpp17::int;\n"
               "using bool = std::cpp17::bool;\n"
               "using float = std::cpp17::float;\n"
               "using double = std::cpp17::double;\n"
               "using size_t = std::cpp17::size_t;\n";
        break;
    case HostLanguage::Rust:
        out << "using i32 = std::rust::i32;\n"
               "using i64 = std::rust::i64;\n"
               "using u32 = std::rust::u32;\n"
               "using u64 = std::rust::u64;\n"
               "using usize = std::rust::usize;\n"
               "using f32 = std::rust::f32;\n"
               "using f64 = std::rust::f64;\n"
               "using bool = std::rust::bool;\n";
        break;
    case HostLanguage::Java:
        // Java type bindings — capitalised Java-idiom names aliased to
        // std::java::*, matching what JavaSymbolExtractor / JavaEmitter
        // resolve and what topo-lang-java/examples/quickstart use.
        // Previously emitted std::cpp17::* verbatim from a copy-paste,
        // producing scaffolded projects that did not type-check.
        out << "using Int = std::java::int;\n"
               "using Boolean = std::java::boolean;\n"
               "using Long = std::java::long;\n"
               "using Double = std::java::double;\n"
               "using String = std::java::String;\n";
        break;
    case HostLanguage::Python:
        out << "using int = std::python::int;\n"
               "using float = std::python::float;\n"
               "using bool = std::python::bool;\n"
               "using str = std::python::str;\n";
        break;
    case HostLanguage::TypeScript:
        out << "using number = std::typescript::number;\n"
               "using string = std::typescript::string;\n"
               "using boolean = std::typescript::boolean;\n";
        break;
    case HostLanguage::Mixed:
        // Mixed projects use C++ type bindings (Rust side uses extern "C")
        out << "using int = std::cpp17::int;\n"
               "using bool = std::cpp17::bool;\n"
               "using float = std::cpp17::float;\n"
               "using double = std::cpp17::double;\n"
               "using size_t = std::cpp17::size_t;\n";
        break;
    }
    return out.str();
}

std::string TopoGenerator::generateTopoBody(const std::vector<HostSymbol>& symbols) const {
    // Step 1: Deduplicate by qualifiedName.
    std::set<std::string> seen;
    std::vector<const HostSymbol*> unique;
    for (auto& sym : symbols) {
        if (seen.insert(sym.qualifiedName).second) unique.push_back(&sym);
    }

    // Step 2+3: Separate and group by namespace. Empty-namespace symbols
    // are wrapped in a sanitized project namespace: the TopoFile grammar
    // allows only imports / data declarations / namespace blocks at the
    // top level, so bare sections or type declarations would not parse.
    const std::string wrapperNs = sanitizeNamespaceName(projectName_);
    std::map<std::string, NamespaceContent> namespaces;

    for (auto* sym : unique) {
        bool isMember = (sym->kind == HostSymbolKind::Method || sym->kind == HostSymbolKind::Constructor ||
                         sym->kind == HostSymbolKind::Destructor || sym->kind == HostSymbolKind::StaticMethod) &&
                        !sym->enclosingClass.empty();

        bool isType = (sym->kind == HostSymbolKind::Class || sym->kind == HostSymbolKind::Struct ||
                       sym->kind == HostSymbolKind::Enum || sym->kind == HostSymbolKind::Interface ||
                       sym->kind == HostSymbolKind::TypeAlias);

        auto [ns, localName] = splitNamespace(sym->qualifiedName, isMember ? sym->enclosingClass : "");
        if (ns.empty()) ns = wrapperNs;

        if (isType) {
            // Register class/struct/enum entry.
            auto& cls = namespaces[ns].classes[localName];
            cls.visibility = effectiveVisibility(*sym);
            cls.hasTodoComment = needsTodoComment(*sym);
        } else if (isMember) {
            std::string className = extractClassName(sym->enclosingClass);
            auto vis = effectiveVisibility(*sym);
            auto& cls = namespaces[ns].classes[className];
            cls.membersByVisibility[vis].push_back(renderSymbol(*sym));
        } else {
            // Free function (or method with empty enclosingClass).
            auto vis = effectiveVisibility(*sym);
            namespaces[ns].freeByVisibility[vis].push_back(renderSymbol(*sym));
        }
    }

    // Step 4: Emit .topo text. A namespace body is strictly visibility
    // sections (NamespaceDecl ::= "namespace" Path "{" { VisibilitySection }
    // "}"), so type blocks are emitted inside their visibility section,
    // never at namespace level.
    std::ostringstream out;

    for (auto& [ns, content] : namespaces) {
        out << "namespace " << ns << " {\n";

        // Gather every visibility level present, canonical order.
        std::vector<Visibility> levels;
        auto addLevel = [&](Visibility v) {
            if (std::find(levels.begin(), levels.end(), v) == levels.end()) levels.push_back(v);
        };
        for (auto& [vis, syms] : content.freeByVisibility) {
            if (!syms.empty()) addLevel(vis);
        }
        for (auto& [className, cls] : content.classes) {
            addLevel(cls.visibility);
        }
        std::sort(levels.begin(), levels.end(), [](Visibility a, Visibility b) {
            return visibilityOrder(a) < visibilityOrder(b);
        });

        for (Visibility vis : levels) {
            indent(out, 1);
            out << visibilitySectionName(vis) << ":\n";

            // Free functions of this visibility.
            auto freeIt = content.freeByVisibility.find(vis);
            if (freeIt != content.freeByVisibility.end()) {
                for (auto& s : freeIt->second) {
                    indent(out, 2);
                    out << s << "\n";
                }
            }

            // Type blocks of this visibility. Members are further grouped
            // into visibility sections inside the type body; an empty body
            // (enum or type with no extracted members) stays legal.
            for (auto& [className, cls] : content.classes) {
                if (cls.visibility != vis) continue;
                indent(out, 2);
                out << "type " << className << " {";
                if (cls.hasTodoComment) out << "  // TODO: verify visibility";
                out << "\n";
                emitVisibilitySections(out, cls.membersByVisibility, 3);
                indent(out, 2);
                out << "}\n";
            }
        }

        out << "}\n\n";
    }

    return out.str();
}

std::string TopoGenerator::generateTopoToml(const std::string& sourcesGlob) const {
    const char* langStr = nullptr;
    switch (lang_) {
    case HostLanguage::Cpp: langStr = "cpp"; break;
    case HostLanguage::Rust: langStr = "rust"; break;
    case HostLanguage::Java: langStr = "java"; break;
    case HostLanguage::Python: langStr = "python"; break;
    case HostLanguage::TypeScript: langStr = "typescript"; break;
    case HostLanguage::Mixed: langStr = "mixed"; break;
    }

    std::ostringstream out;
    out << "[project]\n"
        << "name = \"" << projectName_ << "\"\n"
        << "\n"
        << "[topo]\n"
        << "root = \"topo/main.topo\"\n"
        << "\n"
        << "[build]\n"
        << "language = \"" << langStr << "\"\n"
        << "sources = [\"" << sourcesGlob << "\"]\n"
        << "output = \"" << projectName_ << "\"\n"
        << "\n"
        << "[completeness]\n"
        << "ignore_main = true\n";
    return out.str();
}

} // namespace topo::init
