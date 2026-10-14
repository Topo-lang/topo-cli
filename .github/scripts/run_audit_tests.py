#!/usr/bin/env python3
"""Run required audit suites and reject absent, disabled, skipped or failed tests."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET


def run(build, suite, evidence, platform):
    evidence.mkdir(parents=True, exist_ok=True)
    pattern = {"core": r"^(AuditTest|AnalysisProcessTest)\.",
               "linux-default": r"^(AuditTest|AnalysisProcessTest)\.",
               "linux-experimental": r"^ExperimentalLinuxAnalysisProcessTest\.",
               "cpp": r"^cpp-audit-semantic\.",
               "cli": r"^(AuditSupportTest\.|e2e\.cli\..*audit)"}[suite]
    command = ["ctest", "--test-dir", str(build), "-C", "Release", "-R", pattern]
    listing = subprocess.run(command + ["--show-only=json-v1"], check=True,
                             capture_output=True, text=True)
    (evidence / f"{suite}-registered.json").write_text(listing.stdout)
    tests = json.loads(listing.stdout)["tests"]
    names = {test["name"] for test in tests}
    if suite in ("core", "linux-default"):
        required = {
            "AuditTest.ManualClassRequiresAcceptanceAndRevocationTakesEffect",
            "AuditTest.ChangedSourceInvalidatesApprovalAndSavedReport",
            "AuditTest.SameSimpleNameCannotSelectOrAuthorizeAnotherClass",
            "AuditTest.ProviderWithoutSemanticCapabilityNeverPasses",
        }
        if platform in ("Linux", "Windows"):
            # Linux experimental resource controls do not satisfy the official
            # exact-executable audit contract. Even an opt-in build must refuse.
            required.add("AnalysisProcessTest.UnsupportedPlatformNeverStartsTarget")
        else:
            # Supported backends must execute positive and negative controls.
            required.update({"AnalysisProcessTest.ExactInputFileReadAndOutsideDenial",
                             "AnalysisProcessTest.LoopbackConnectionIsActuallyDenied"})
        if suite == "linux-default":
            if platform != "Linux":
                raise RuntimeError("Linux default-off verification requires a Linux runner")
            required.add("AnalysisProcessTest.ExperimentalLinuxBackendDisabledByDefault")
    elif suite == "linux-experimental":
        if platform != "Linux":
            raise RuntimeError("Linux experimental controls require a Linux runner")
        required = {"ExperimentalLinuxAnalysisProcessTest." + name for name in (
            "ExactInputFileReadAndOutsideDenial", "LoopbackConnectionIsActuallyDenied",
            "UnlistedInheritedWritableDescriptorCannotModifyOutsideFile",
            "ReadableImagesCanRunThroughBoundElfLoader")}
    elif suite == "cpp":
        required = {
            "cpp-audit-semantic.CppAudit.PreventsActualNativeGpuHelperExecution",
            "cpp-audit-semantic.CppAudit.RejectsGpuDiscoveryBeforeParsingAndExpandsResponseOrder",
            "cpp-audit-semantic.CppAudit.DistinguishesSameNamedClassesAndBindsOutOfLineDefinition",
            "cpp-audit-semantic.CppAudit.VirtualAndFunctionPointerCallsStayUnverified",
        }
    else:
        required = {"e2e.cli." + name for name in (
            "cpp_audit", "cpp_audit_headers", "cpp_audit_macro_headers",
            "cpp_audit_isolation", "cpp_audit_parent_inputs", "cpp_audit_path_semantics",
            "java_audit", "java_audit_isolation", "java_build_audit")}
        required.update({"AuditSupportTest.AcceptanceAndRevocationPersistOutsideSubject",
                         "AuditSupportTest.FactsCacheRoundTripBindsIdentityAndRetainsDiagnostics"})
    missing = required - names
    disabled = [test["name"] for test in tests for prop in test.get("properties", [])
                if prop["name"] == "DISABLED" and prop["value"]]
    if missing or disabled:
        raise RuntimeError(f"Missing required tests: {sorted(missing)}; disabled tests: {disabled}")
    junit = evidence / f"{suite}-junit.xml"
    result = subprocess.run(command + ["--no-tests=error", "--output-on-failure",
                                      "--output-junit", str(junit)], check=False)
    if not junit.exists():
        raise RuntimeError(f"CTest produced no result file (exit {result.returncode})")
    cases = ET.parse(junit).getroot().findall(".//testcase")
    executed = {case.attrib["name"] for case in cases}
    unsuccessful = [case.attrib["name"] for case in cases
                    if case.find("skipped") is not None or case.find("failure") is not None
                    or case.find("error") is not None
                    or case.attrib.get("status", "run") != "run"]
    skipped = sum(case.find("skipped") is not None for case in cases)
    failed = sum(case.find("failure") is not None or case.find("error") is not None for case in cases)
    summary = {"suite": suite, "platform": platform, "registered": len(tests),
               "executed": len(cases), "passed": len(cases) - len(unsuccessful),
               "failed": failed, "skipped": skipped, "unsuccessful": unsuccessful,
               "missingResults": sorted(names - executed), "ctestExit": result.returncode}
    (evidence / f"{suite}-summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)
    if result.returncode or unsuccessful or executed != names:
        raise RuntimeError("Audit suite contains failed, skipped or missing results")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--suite", choices=("core", "linux-default", "linux-experimental", "cpp", "cli"), required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--platform", choices=("macOS", "Linux", "Windows"), required=True)
    args = parser.parse_args()
    try:
        run(args.build.resolve(), args.suite, args.evidence.resolve(), args.platform)
    except (RuntimeError, subprocess.CalledProcessError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
