#ifndef TOPO_CLI_AUDITSUPPORT_H
#define TOPO_CLI_AUDITSUPPORT_H

#include "topo/Check/Audit.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace topo {
struct CheckConfig;
check::AuditPolicy loadAuditPolicy(const std::string& path);
std::string auditTrustPath(const CheckConfig& config);
nlohmann::json loadAuditTrust(const CheckConfig& config);
int updateAuditTrust(const CheckConfig& config);
void saveAuditReport(const std::string& path, const nlohmann::json& report);
bool auditCacheEnabled(const CheckConfig& config);
nlohmann::json auditCacheContext(const CheckConfig& config, const std::vector<std::string>& inputs);
bool loadAuditFactsCache(const CheckConfig& config, const nlohmann::json& identity,
                        nlohmann::json& payload, std::string& reason);
bool saveAuditFactsCache(const CheckConfig& config, const nlohmann::json& identity,
                        const nlohmann::json& payload, std::string& reason);
} // namespace topo
#endif
