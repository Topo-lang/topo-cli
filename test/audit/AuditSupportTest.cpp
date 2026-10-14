#include "AuditSupport.h"
#include "CheckRunner.h"
#include "topo/Check/Audit.h"
#include "topo/Platform/TempFile.h"
#include "topo/Platform/Process.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#ifdef __APPLE__
#include <pwd.h>
#include <unistd.h>
#endif
namespace {
namespace fs=std::filesystem;
class AuditSupportTest: public ::testing::Test {
protected:
 topo::platform::TempFile marker{"audit-trust"};
 fs::path root=marker.path().string()+"-root";
 topo::CheckConfig config;
 void SetUp() override {
  fs::create_directories(root/"project"); fs::create_directory(root/"user");
  fs::permissions(root/"user",fs::perms::owner_all);
  config.projectDir=(root/"project").string(); config.trustStore=(root/"user/trust.json").string();
  std::ofstream(root/"project/A.java") << "class A {}";
 }
 void TearDown() override { std::error_code ec; fs::remove_all(root,ec); }
 nlohmann::json report() {
  topo::check::AuditFacts f; f.supported=f.complete=true; f.provider="test";
  f.identities.push_back({"a","A","class","","",(root/"project/A.java").string(),1,"project",true});
  return topo::check::makeAuditReport(config.projectDir,f,{}, {"A"},{(root/"project/A.java").string()},{});
 }
};
TEST_F(AuditSupportTest, ProjectCannotHostItsOwnApprovals) {
 config.trustStore=(root/"project/trust.json").string();
 EXPECT_THROW(topo::loadAuditTrust(config),std::runtime_error);
}
TEST_F(AuditSupportTest, PolicyUnknownKeysAndWrongTypesReject) {
 auto path=root/"policy.toml"; std::ofstream(path) << "strict_dependencies = 'yes'\n";
 EXPECT_THROW(topo::loadAuditPolicy(path.string()),std::runtime_error);
 std::ofstream(path) << "skip_all = true\n";
 EXPECT_THROW(topo::loadAuditPolicy(path.string()),std::runtime_error);
}
#ifndef _WIN32
TEST_F(AuditSupportTest, AcceptanceAndRevocationPersistOutsideSubject) {
 auto r=report(); auto file=root/"report.json"; topo::saveAuditReport(file.string(),r);
 config.acceptReview=file.string(); ASSERT_EQ(topo::updateAuditTrust(config),0);
 auto trust=topo::loadAuditTrust(config); topo::check::applyAuditTrust(r,trust); EXPECT_TRUE(r["passed"].get<bool>());
 config.acceptReview.clear(); config.revokeReview=r["reviewKey"].get<std::string>();
 ASSERT_EQ(topo::updateAuditTrust(config),0); topo::check::applyAuditTrust(r,topo::loadAuditTrust(config)); EXPECT_FALSE(r["passed"].get<bool>());
}
TEST_F(AuditSupportTest, SourceReplacementRefusesStaleApproval) {
 auto r=report(); auto file=root/"report.json"; topo::saveAuditReport(file.string(),r);
 std::ofstream(root/"project/A.java") << "class A { static { hidden(); } }";
 config.acceptReview=file.string(); EXPECT_EQ(topo::updateAuditTrust(config),2);
 EXPECT_FALSE(fs::exists(config.trustStore));
}
TEST_F(AuditSupportTest, WritableStoreAndSymlinkLockReject) {
 std::ofstream(config.trustStore) << "{}";
 fs::permissions(config.trustStore,fs::perms::owner_all|fs::perms::others_write);
 EXPECT_THROW(topo::loadAuditTrust(config),std::runtime_error);
 fs::remove(config.trustStore); fs::create_symlink(root/"project/A.java",config.trustStore+".lock");
 config.revokeReview="key"; EXPECT_EQ(topo::updateAuditTrust(config),2);
}
TEST_F(AuditSupportTest, FactsCacheRoundTripBindsIdentityAndRetainsDiagnostics) {
 config.auditCacheDir=(root/"user/cache").string();
 nlohmann::json identity={{"provider","test"},{"model","first"}};
 nlohmann::json payload={{"facts",{{"complete",false}}},{"diagnostics",{"unresolved input"}}}, restored;
 std::string reason;
 ASSERT_TRUE(topo::saveAuditFactsCache(config,identity,payload,reason)) << reason;
 ASSERT_TRUE(topo::loadAuditFactsCache(config,identity,restored,reason)) << reason;
 EXPECT_EQ(payload,restored);
 identity["model"]="changed";
 EXPECT_FALSE(topo::loadAuditFactsCache(config,identity,restored,reason));
}
TEST_F(AuditSupportTest, FactsCacheRejectsSubjectStoreAndTamperedPayload) {
 nlohmann::json identity={{"provider","test"}}, payload={{"facts","complete"}}, restored;
 std::string reason;
 config.auditCacheDir=(root/"project/cache").string();
 EXPECT_FALSE(topo::saveAuditFactsCache(config,identity,payload,reason));
 EXPECT_NE(reason.find("outside"),std::string::npos);
 config.auditCacheDir=(root/"user/cache").string();
 ASSERT_TRUE(topo::saveAuditFactsCache(config,identity,payload,reason)) << reason;
 for (const auto& entry:fs::directory_iterator(config.auditCacheDir)) if (entry.path().extension()==".json") {
  nlohmann::json data; std::ifstream(entry.path()) >> data;
  data["payload"]={{"facts","forged PASS"}};
  std::ofstream(entry.path()) << data;
 }
 EXPECT_FALSE(topo::loadAuditFactsCache(config,identity,restored,reason));
 EXPECT_NE(reason.find("mismatch"),std::string::npos);
}
TEST_F(AuditSupportTest, FactsCacheRejectsUnprotectedFilesAndSymlinkDirectory) {
 nlohmann::json identity={{"provider","test"}}, payload={{"facts","complete"}}, restored;
 std::string reason;
 config.auditCacheDir=(root/"user/cache").string();
 ASSERT_TRUE(topo::saveAuditFactsCache(config,identity,payload,reason)) << reason;
 for (const auto& entry:fs::directory_iterator(config.auditCacheDir)) if (entry.path().extension()==".json")
  fs::permissions(entry.path(),fs::perms::others_write,fs::perm_options::add);
 EXPECT_FALSE(topo::loadAuditFactsCache(config,identity,restored,reason));
 fs::create_directory_symlink(root/"user/cache",root/"user/link");
 config.auditCacheDir=(root/"user/link").string();
 EXPECT_FALSE(topo::saveAuditFactsCache(config,identity,payload,reason));
}
TEST_F(AuditSupportTest, FactsCacheResolvesAncestorAliasesIntoProtectedDirectory) {
 fs::create_directory_symlink(root/"user",root/"alias");
 config.auditCacheDir=(root/"alias/cache").string();
 const nlohmann::json identity={{"provider","test"}}, payload={{"facts","complete"}};
 std::string reason;
 ASSERT_TRUE(topo::saveAuditFactsCache(config,identity,payload,reason)) << reason;
 EXPECT_TRUE(fs::is_directory(root/"user/cache"));
 fs::remove(root/"alias");
 config.auditCacheDir=(root/"user/cache").string();
 nlohmann::json restored;
 EXPECT_TRUE(topo::loadAuditFactsCache(config,identity,restored,reason)) << reason;
 EXPECT_EQ(restored,payload);
}
#ifdef __APPLE__
TEST_F(AuditSupportTest, ExtendedAclOtherWriterRejectsTrustAndCache) {
 std::ofstream(config.trustStore) << R"({"schemaVersion":1,"reviews":{},"dependencies":{}})";
 fs::permissions(config.trustStore,fs::perms::owner_read|fs::perms::owner_write,fs::perm_options::replace);
 ASSERT_EQ(topo::platform::runProcess("/bin/chmod",{"+a","everyone allow write",config.trustStore}).exitCode,0);
 EXPECT_THROW(topo::loadAuditTrust(config),std::runtime_error);
 config.auditCacheDir=(root/"user/cache").string(); fs::create_directory(config.auditCacheDir);
 ASSERT_EQ(topo::platform::runProcess("/bin/chmod",{"+a","everyone allow add_file",config.auditCacheDir}).exitCode,0);
 std::string reason;
 EXPECT_FALSE(topo::saveAuditFactsCache(config,{{"provider","test"}},{{"facts","complete"}},reason));
 EXPECT_NE(reason.find("ACL"),std::string::npos);
}
TEST_F(AuditSupportTest, ExtendedOwnerAclPreservesProtectedStore) {
 std::ofstream(config.trustStore) << R"({"schemaVersion":1,"reviews":{},"dependencies":{}})";
 auto* user=getpwuid(geteuid()); ASSERT_NE(user,nullptr);
 const std::string grant=std::string(user->pw_name)+" allow write";
 ASSERT_EQ(topo::platform::runProcess("/bin/chmod",{"+a",grant,config.trustStore}).exitCode,0);
 EXPECT_NO_THROW(topo::loadAuditTrust(config));
}
#endif
#endif
}
