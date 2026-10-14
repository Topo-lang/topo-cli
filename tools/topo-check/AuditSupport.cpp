#include "AuditSupport.h"
#include "CheckRunner.h"
#include "topo/Platform/TempFile.h"
#include "topo/Platform/FileLock.h"
#include "topo/Platform/SharedLibrary.h"
#include "topo/Distribution/Sha256.h"

#include <toml++/toml.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <cerrno>
#if defined(__APPLE__)
#include <crt_externs.h>
#include <sys/acl.h>
#include <membership.h>
#include <cstring>
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <link.h>
extern char** environ;
#elif !defined(_WIN32)
extern char** environ;
#endif
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace topo {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

#ifndef _WIN32
void requireProtectedAcl(const fs::path& path) {
#ifdef __APPLE__
    acl_t acl = acl_get_file(path.c_str(), ACL_TYPE_EXTENDED);
    if (!acl) {
        const int error = errno;
        struct stat info{};
        if (error == ENOENT && ::lstat(path.c_str(), &info) == 0) return;
        throw std::runtime_error("cannot inspect audit store ACL: " + path.string());
    }
    uuid_t owner{}, root{};
    if (mbr_uid_to_uuid(geteuid(), owner) != 0 || mbr_uid_to_uuid(0, root) != 0) {
        acl_free(acl);
        throw std::runtime_error("cannot identify audit store ACL owner");
    }
    bool unsafe = false;
    acl_entry_t entry;
    for (int next = ACL_FIRST_ENTRY; acl_get_entry(acl, next, &entry) == 0; next = ACL_NEXT_ENTRY) {
        acl_tag_t tag;
        acl_permset_t permissions;
        if (acl_get_tag_type(entry, &tag) != 0 || acl_get_permset(entry, &permissions) != 0) { unsafe = true; break; }
        if (tag != ACL_EXTENDED_ALLOW) continue;
        void* principal = acl_get_qualifier(entry);
        const bool trusted = principal && (std::memcmp(principal, owner, sizeof(owner)) == 0 || std::memcmp(principal, root, sizeof(root)) == 0);
        if (principal) acl_free(principal);
        if (trusted) continue;
        for (auto permission : {ACL_WRITE_DATA, ACL_APPEND_DATA, ACL_DELETE, ACL_DELETE_CHILD,
                                ACL_WRITE_ATTRIBUTES, ACL_WRITE_EXTATTRIBUTES, ACL_WRITE_SECURITY, ACL_CHANGE_OWNER})
            if (acl_get_perm_np(permissions, permission) != 0) unsafe = true;
        if (unsafe) break;
    }
    acl_free(acl);
    if (unsafe) throw std::runtime_error("audit store ACL allows other writers: " + path.string());
#else
    (void)path;
#endif
}
#endif

void requireProtectedPath(const fs::path& path, bool directory) {
#ifdef _WIN32
    (void)path; (void)directory;
    throw std::runtime_error("protected audit trust requires an ownership/ACL verifier; unavailable on this platform");
#else
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0 || info.st_uid != ::geteuid() ||
        (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode)) ||
        (info.st_mode & (S_IWGRP | S_IWOTH)))
        throw std::runtime_error("audit trust path must be user-owned and protected from other writers: " + path.string());
    requireProtectedAcl(path);
    for (auto parent=path.parent_path(); !parent.empty(); parent=parent.parent_path()) {
        if (::stat(parent.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) ||
            (info.st_uid != ::geteuid() && info.st_uid != 0))
            throw std::runtime_error("untrusted audit store ancestor: " + parent.string());
        // The OS temporary directory may be shared; its sticky bit protects
        // the user-owned child from replacement by another account.
        if ((info.st_mode & (S_IWGRP | S_IWOTH)) && !(info.st_uid==0 && (info.st_mode & S_ISVTX)))
            throw std::runtime_error("audit store ancestor allows other writers: " + parent.string());
        requireProtectedAcl(parent);
        if (parent==parent.root_path()) break;
    }
#endif
}

bool within(const fs::path& root, const fs::path& file) {
    auto r = fs::weakly_canonical(root);
    auto f = fs::weakly_canonical(file);
    auto ri = r.begin(), fi = f.begin();
    for (; ri != r.end(); ++ri, ++fi) if (fi == f.end() || *ri != *fi) return false;
    return true;
}
json readJson(const fs::path& path) {
    if (!fs::is_regular_file(path) || fs::file_size(path) > 32 * 1024 * 1024) throw std::runtime_error("audit JSON must be a regular file within 32 MiB");
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    return json::parse(in);
}
void writeJson(const fs::path& path, const json& value) {
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    // Exclusive directories are used for the protected trust store. The
    // report is public data; neither is ever considered a signed statement.
    platform::TempFile temporary(path.parent_path().empty() ? fs::path(".") : path.parent_path(),
                                 "topo-audit", ".json");
    const auto& temp = temporary.path();
    {
        std::ofstream out(temp, std::ios::binary);
        if (!out || !(out << value.dump(2) << '\n') || !out.flush()) {
            std::error_code ec; fs::remove(temp, ec);
            throw std::runtime_error("cannot write audit file: " + path.string());
        }
    }
    platform::atomicReplaceFile(temp, path);
    temporary.release();
}
std::vector<std::string> stringArray(const toml::table& t, const char* name) {
    std::vector<std::string> result;
    if (auto a = t[name].as_array()) {
        for (const auto& e : *a) {
            auto s = e.value<std::string>();
            if (!s || s->empty()) throw std::runtime_error(std::string(name) + " requires nonempty strings");
            result.push_back(*s);
        }
    } else if (t.contains(name)) throw std::runtime_error(std::string(name) + " must be an array");
    return result;
}

fs::path cacheDirectory(const CheckConfig& config) {
    auto path = config.auditCacheDir;
    if (path.empty()) if (const auto* env = std::getenv("TOPO_AUDIT_CACHE_DIR")) path = env;
    if (path.empty()) throw std::runtime_error("no explicit reviewer audit cache directory");
    fs::path directory = fs::absolute(path).lexically_normal();
    if (within(config.projectDir, directory)) throw std::runtime_error("audit cache must be outside the subject project");
    if (fs::is_symlink(fs::symlink_status(directory))) throw std::runtime_error("audit cache directory must not be a symlink");
    // Continue through the resolved directory, so replacing an ancestor
    // symlink after validation cannot redirect a protected read/write.
    return fs::weakly_canonical(directory);
}
std::string cacheKey(const json& identity) {
    return dist::sha256Hex(json{{"kind", "topo.audit-facts-cache"}, {"schemaVersion", 1}, {"identity", identity}}.dump());
}
} // namespace

bool auditCacheEnabled(const CheckConfig& config) {
    const auto* env = std::getenv("TOPO_AUDIT_CACHE_DIR");
    return !config.auditCacheDir.empty() || (env && *env);
}

nlohmann::json auditCacheContext(const CheckConfig& config, const std::vector<std::string>& files) {
    json inputs = json::object();
    auto capture = [&](const fs::path& path) {
        const auto name = fs::absolute(path).lexically_normal().generic_string();
        if (inputs.contains(name)) return;
        if (!fs::is_regular_file(path) || fs::file_size(path) > 1024ULL * 1024 * 1024)
            throw std::runtime_error("cache input unavailable or oversized: " + path.string());
        auto hash = dist::sha256File(path.string());
        if (hash.empty()) throw std::runtime_error("cannot hash cache input: " + path.string());
        inputs[name] = {{"resolvedPath", fs::canonical(path).generic_string()}, {"sha256", hash}};
    };
    for (const auto& path : files) if (!path.empty()) capture(path);
    capture(fs::path(platform::getExecutableDir()) / "topo-check");
    // Provider/catalog code can itself be dynamically linked into the CLI.
    // Bind those images as well as the extractor's separately probed runtime.
    std::vector<std::string> images;
#ifdef __APPLE__
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) images.emplace_back(_dyld_get_image_name(i));
#elif defined(__linux__)
    dl_iterate_phdr([](struct dl_phdr_info* image, size_t, void* data) {
        static_cast<std::vector<std::string>*>(data)->emplace_back(*image->dlpi_name ? image->dlpi_name : "/proc/self/exe");
        return 0;
    }, &images);
#else
    throw std::runtime_error("audit cache runtime inventory unavailable on this platform");
#endif
    if (images.empty() || images.size() > 512) throw std::runtime_error("audit cache runtime inventory unavailable or oversized");
    for (const auto& image : images) {
#ifdef __APPLE__
        // The provider probe binds these sealed images to the running OS build.
        if (image.rfind("/usr/lib/", 0) == 0 || image.rfind("/System/Library/", 0) == 0 || image.rfind("/System/Cryptexes/OS/", 0) == 0) continue;
#else
        if (image == "linux-vdso.so.1" || image == "linux-gate.so.1") continue;
#endif
        if (within(config.projectDir, image)) throw std::runtime_error("audit cache runtime image is inside the subject project");
        capture(image);
    }
    // Store only a digest of the environment; values can contain credentials.
    std::map<std::string, std::string> environment;
#ifdef __APPLE__
    for (char** entry = *_NSGetEnviron(); *entry; ++entry) environment.emplace(*entry, "");
#elif !defined(_WIN32)
    for (char** entry = environ; *entry; ++entry) environment.emplace(*entry, "");
#else
    throw std::runtime_error("audit cache environment identity unavailable on this platform");
#endif
    return {{"project", fs::canonical(config.projectDir).generic_string()}, {"inputs", inputs},
            {"environmentDigest", dist::sha256Hex(json(environment).dump())},
            {"workingDirectory", fs::current_path().generic_string()}, {"nativeRoots", config.evidenceRoots}};
}

bool loadAuditFactsCache(const CheckConfig& config, const json& identity, json& payload, std::string& reason) {
    try {
        auto directory = cacheDirectory(config);
        if (!fs::exists(directory)) { reason = "no cached facts"; return false; }
        requireProtectedPath(directory, true);
        const auto path = directory / (cacheKey(identity) + ".json");
        if (!fs::exists(fs::symlink_status(path))) { reason = "no matching cached facts"; return false; }
        requireProtectedPath(path, false);
        const auto data = readJson(path);
        if (data.at("kind") != "topo.audit-facts-cache" || data.at("schemaVersion") != 1 || data.at("identity") != identity ||
            data.at("payloadDigest") != dist::sha256Hex(data.at("payload").dump()))
            throw std::runtime_error("audit cache identity or payload mismatch");
        payload = data.at("payload");
        reason.clear();
        return true;
    } catch (const std::exception& error) { reason = error.what(); return false; }
}

bool saveAuditFactsCache(const CheckConfig& config, const json& identity, const json& payload, std::string& reason) {
    try {
        if (payload.is_null()) { reason = "provider facts are incomplete or unsuitable for reuse"; return false; }
        auto directory = cacheDirectory(config);
        if (fs::create_directories(directory)) fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace);
        requireProtectedPath(directory, true);
        const auto path = directory / (cacheKey(identity) + ".json");
        const auto lockPath = fs::path(path.string() + ".lock");
        if (fs::exists(fs::symlink_status(lockPath))) requireProtectedPath(lockPath, false);
        platform::FileLock lock(lockPath);
        if (!lock.tryAcquire()) throw std::runtime_error("audit cache is busy or inaccessible");
        requireProtectedPath(lockPath, false);
        if (fs::exists(fs::symlink_status(path))) requireProtectedPath(path, false);
        const json data = {{"kind", "topo.audit-facts-cache"}, {"schemaVersion", 1}, {"identity", identity},
                           {"payload", payload}, {"payloadDigest", dist::sha256Hex(payload.dump())}};
        if (data.dump().size() > 32 * 1024 * 1024) throw std::runtime_error("audit cache payload exceeds 32 MiB");
        writeJson(path, data);
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
        reason.clear();
        return true;
    } catch (const std::exception& error) { reason = error.what(); return false; }
}

check::AuditPolicy loadAuditPolicy(const std::string& path) {
    check::AuditPolicy p;
    if (path.empty()) return p;
    const auto t = toml::parse_file(path);
    for (const auto& [key, value] : t) {
        const auto k = key.str();
        if (k != "version" && k != "dependencies" && k != "strict_dependencies" &&
            k != "manual" && k != "expand" && k != "deny")
            throw std::runtime_error("unknown audit policy key: " + std::string(k));
        (void)value;
    }
    if ((t.contains("version") && !t["version"].is_integer()) ||
        (t.contains("dependencies") && !t["dependencies"].is_string()) ||
        (t.contains("strict_dependencies") && !t["strict_dependencies"].is_boolean()))
        throw std::runtime_error("invalid audit policy value type");
    p.schemaVersion = t["version"].value_or(1);
    p.dependencies = t["dependencies"].value_or(std::string("opaque"));
    p.strictDependencies = t["strict_dependencies"].value_or(false);
    p.expand = stringArray(t, "expand");
    p.deny = stringArray(t, "deny");
    if (auto rules = t["manual"].as_array()) {
        for (const auto& item : *rules) {
            const auto* rule = item.as_table();
            if (!rule) throw std::runtime_error("manual rules must be tables");
            for (const auto& [key, value] : *rule) {
                if (key.str() != "kind" && key.str() != "name" && key.str() != "file")
                    throw std::runtime_error("unknown manual selector key: " + std::string(key.str()));
                (void)value;
            }
            check::AuditSelector s;
            for (const char* key : {"kind", "name", "file"})
                if (rule->contains(key) && !(*rule)[key].is_string())
                    throw std::runtime_error(std::string("manual selector ") + key + " must be a string");
            s.kind = (*rule)["kind"].value_or(std::string());
            s.name = (*rule)["name"].value_or(std::string());
            s.file = (*rule)["file"].value_or(std::string());
            if ((s.kind != "class" && s.kind != "file") || (s.kind == "class" && s.name.empty()) || (s.kind == "file" && s.file.empty()))
                throw std::runtime_error("manual selector requires class/name or file/file");
            p.manual.push_back(std::move(s));
        }
    } else if (t.contains("manual")) throw std::runtime_error("manual must be an array of tables");
    return p;
}

std::string auditTrustPath(const CheckConfig& c) {
    if (c.trustStore.empty() && platform::homeDirectory().empty())
        throw std::runtime_error("user home unavailable; specify --trust-store outside the subject project");
    fs::path file = c.trustStore.empty() ? platform::homeDirectory() / ".topo/audit/trust.json" : fs::path(c.trustStore);
    if (file.empty() || within(c.projectDir, file))
        throw std::runtime_error("audit trust store must be outside the subject project");
    if (fs::is_symlink(fs::symlink_status(file)))
        throw std::runtime_error("audit trust store must be a regular user-owned file");
    return fs::weakly_canonical(file).string();
}

nlohmann::json loadAuditTrust(const CheckConfig& c) {
    auto file = auditTrustPath(c);
    if (!fs::exists(file)) return json{{"schemaVersion", 1}, {"reviews", json::object()}, {"dependencies", json::object()}};
    requireProtectedPath(fs::path(file).parent_path(), true);
    requireProtectedPath(file, false);
    auto trust = readJson(file);
    if (trust.value("schemaVersion", 0) != 1 || !trust.at("reviews").is_object() || !trust.at("dependencies").is_object())
        throw std::runtime_error("unsupported audit trust store");
    return trust;
}

void saveAuditReport(const std::string& path, const nlohmann::json& report) {
    if (!path.empty()) writeJson(path, report);
}

int updateAuditTrust(const CheckConfig& c) {
    try {
        if (c.trustScope != "project" && c.trustScope != "user") throw std::runtime_error("trust scope must be project or user");
        const fs::path path(auditTrustPath(c));
        std::error_code ec;
        if (fs::create_directories(path.parent_path())) {
            fs::permissions(path.parent_path(), fs::perms::owner_all, fs::perm_options::replace, ec);
            if (ec) throw std::runtime_error("cannot protect audit trust directory");
        }
        requireProtectedPath(path.parent_path(), true);
        const auto lockPath=fs::path(path.string()+".lock");
        if (fs::exists(fs::symlink_status(lockPath))) requireProtectedPath(lockPath, false);
        platform::FileLock lock(lockPath);
        if (!lock.tryAcquire()) throw std::runtime_error("audit trust store is busy or inaccessible; retry after the current update");
        requireProtectedPath(lockPath, false);
        auto trust = loadAuditTrust(c);
        if (!c.revokeReview.empty()) {
            trust["reviews"].erase(c.revokeReview);
            trust["dependencies"].erase(c.revokeReview);
        } else {
            auto report = readJson(c.acceptReview.empty() ? c.auditReportPath : c.acceptReview);
            if (report.contains("audit")) report = report.at("audit");
            std::string error;
            if (!check::verifyAuditSnapshot(report, error)) throw std::runtime_error(error);
            if (fs::canonical(c.projectDir).generic_string() != report.at("project").get<std::string>())
                throw std::runtime_error("report belongs to a different project; select its --project explicitly");
            if (!c.acceptDependency.empty()) {
                bool found = false;
                for (const auto& dep : report.at("dependencies")) if (dep.at("key") == c.acceptDependency) {
                    if (dep.at("approval") == "revoked") throw std::runtime_error("dependency is denied by project policy");
                    trust["dependencies"][c.acceptDependency] = {{"accepted", true}, {"identity", dep},
                        {"project", c.trustScope == "user" ? "" : report.at("project").get<std::string>()}};
                    found = true;
                }
                if (!found) throw std::runtime_error("dependency key is absent from this report");
            } else {
                if (report.at("manual").empty()) throw std::runtime_error("report has no manual boundaries to accept");
                trust["reviews"][report.at("reviewKey").get<std::string>()] = {{"accepted", true}, {"project", report.at("project")}};
            }
        }
        writeJson(path, trust);
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
        if (ec) throw std::runtime_error("cannot protect audit trust file");
        std::cout << "Audit trust record updated.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "topo-check: " << e.what() << '\n';
        return 2;
    }
}
} // namespace topo
