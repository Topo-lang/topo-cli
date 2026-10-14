#!/usr/bin/env python3
"""Build the focused audit install chain from exact dependency checkouts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def execute(command):
    print("+ " + subprocess.list2cmdline([str(arg) for arg in command]), flush=True)
    subprocess.run([str(arg) for arg in command], check=True)


def record_toolchain(evidence, java_home, llvm, extractor, work, prefix):
    tools = {}
    for name, path in {"cppExtractor": extractor, "check": work / "topo-cli/tools/topo-check/topo-check",
                       "build": work / "topo-cli/tools/topo-build/topo-build",
                       "javaBackend": prefix / "bin/topo-build-jvm-java"}.items():
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        tools[name] = {"path": str(path), "sha256": digest.hexdigest()}
    versions = {}
    for name, command in {"clang": [llvm / "bin/clang++", "--version"],
                          "java": [java_home / "bin/java", "-version"]}.items():
        result = subprocess.run(command, check=True, capture_output=True, text=True)
        versions[name] = (result.stdout + result.stderr).strip()
    (evidence / "toolchain.json").write_text(json.dumps({
        "javaHome": str(java_home), "llvmRoot": str(llvm),
        "versions": versions, "tools": tools,
        "installPrefix": str(prefix)}, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sources", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--platform", choices=("macOS", "Linux", "Windows"), required=True)
    parser.add_argument("--toolchain", type=Path)
    parser.add_argument("--prefix-path", default="")
    parser.add_argument("--cmake-option", action="append", default=[])
    parser.add_argument("--jobs", type=int, default=3)
    args = parser.parse_args()
    cli = Path(__file__).resolve().parents[2]
    sources = args.sources.resolve()
    work = args.work.resolve()
    prefix = work / "install"
    evidence = work / "evidence"
    evidence.mkdir(parents=True, exist_ok=True)
    common = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
              f"-DCMAKE_INSTALL_PREFIX={prefix}",
              f"-DCMAKE_PREFIX_PATH={prefix};{args.prefix_path}", *args.cmake_option]
    if args.platform == "Windows":
        # Match the standalone core Windows lane and its runtime DLLs.
        # GTest must use the same ABI instead of vcpkg's default MSVC triplet.
        common += ["-DCMAKE_C_COMPILER=C:/mingw64/bin/gcc.exe",
                   "-DCMAKE_CXX_COMPILER=C:/mingw64/bin/g++.exe",
                   "-DVCPKG_TARGET_TRIPLET=x64-mingw-static"]
    if args.toolchain:
        common += [f"-DCMAKE_TOOLCHAIN_FILE={args.toolchain.resolve()}",
                   f"-DVCPKG_MANIFEST_DIR={cli / '.github/audit-deps'}",
                   f"-DVCPKG_INSTALLED_DIR={work / 'vcpkg_installed'}"]

    def configure(name, *options, source=None):
        execute(["cmake", "-S", source or sources / name, "-B", work / name,
                 *common, *options])

    def build(name, *targets):
        execute(["cmake", "--build", work / name, "--parallel", args.jobs,
                 "--target", *targets])

    def test(name, suite):
        execute([sys.executable, cli / ".github/scripts/run_audit_tests.py",
                 "--build", work / name, "--suite", suite, "--platform", args.platform,
                 "--evidence", evidence])

    # Verify the default-disabled build first. Experimental resource controls
    # are tested separately below and do not enable official Linux audit.
    configure("topo-core", "-DTOPO_CORE_BUILD_TESTS=OFF",
              "-U", "TOPO_ENABLE_EXPERIMENTAL_LINUX_ANALYSIS")
    if args.platform == "Linux":
        cache = (work / "topo-core/CMakeCache.txt").read_text().splitlines()
        if "TOPO_ENABLE_EXPERIMENTAL_LINUX_ANALYSIS:BOOL=OFF" not in cache:
            raise RuntimeError("The Linux experimental backend must default to OFF")
    build("topo-core", "install")
    configure("core-tests", f"-DTOPO_CORE_SOURCE={sources / 'topo-core'}",
              source=cli / ".github/audit-core")
    build("core-tests", "topo-audit-contract-tests", "topo-analysis-process-tests")
    test("core-tests", "linux-default" if args.platform == "Linux" else "core")
    if args.platform == "Linux":
        # Reinstall with the low-level API available and rebuild its consumers
        # so the installed target's PUBLIC compile definition is also checked.
        configure("topo-core", "-DTOPO_CORE_BUILD_TESTS=OFF",
                  "-DTOPO_ENABLE_EXPERIMENTAL_LINUX_ANALYSIS=ON")
        build("topo-core", "install")
        configure("core-tests", f"-DTOPO_CORE_SOURCE={sources / 'topo-core'}",
                  source=cli / ".github/audit-core")
        build("core-tests", "topo-audit-contract-tests", "topo-analysis-process-tests")
        # Even this opt-in build must refuse the official exact-executable
        # audit contract; the loader counterexample has its own result gate.
        test("core-tests", "core")
        test("core-tests", "linux-experimental")
    if args.platform != "macOS":
        return

    # Install current source afresh. Upstream install trees are never cached.
    configure("topo-lang", "-DTOPO_LANG_BUILD_TESTS=OFF")
    build("topo-lang", "install")
    # The language-dependent core libraries become available after topo-lang.
    configure("topo-core", "-DTOPO_CORE_BUILD_TESTS=OFF")
    build("topo-core", "install")
    configure("topo-lang-cpp", "-DTOPO_LANG_CPP_BUILD_TESTS=OFF",
              "-DTOPO_LANG_CPP_ENABLE_LLVM=OFF")
    build("topo-lang-cpp", "install")

    java_home = Path(os.environ["JAVA_HOME"]).resolve(strict=True)
    configure("topo-lang-java", "-DTOPO_LANG_JAVA_BUILD_TESTS=OFF",
              "-DTOPO_LANG_JAVA_ENABLE_JVM_BACKEND=ON",
              f"-DTOPO_GRADLE_JAVA_HOME={java_home}",
              "-DTOPO_LANG_JAVA_GRADLE_ARGS=--no-daemon --console=plain")
    build("topo-lang-java", "install")
    configure("topo-jvm", "-DTOPO_JVM_BUILD_TESTS=OFF", "-DTOPO_JVM_BUILD_E2E=OFF",
              "-DTOPO_JVM_BUILD_TOOLS=ON", "-DTOPO_JVM_BUILD_TRANSFORM=ON",
              "-DTOPO_JVM_BUILD_DECOMPILE=OFF", f"-DTOPO_GRADLE_JAVA_HOME={java_home}",
              "-DTOPO_JVM_GRADLE_ARGS=--no-daemon --console=plain")
    build("topo-jvm", "install")
    # The backend locates its runtime and transformer beside its executable.
    shutil.copy2(prefix / "share/topo-lang-java/lib/topo-runtime.jar", prefix / "bin/topo-runtime.jar")
    shutil.copy2(prefix / "share/topo-jvm/lib/topo-transform.jar", prefix / "bin/topo-transform.jar")

    llvm = Path(os.environ["TOPO_AUDIT_LLVM_ROOT"]).resolve(strict=True)
    configure("extractor", f"-DTOPO_CPP_SOURCE={sources / 'topo-lang-cpp'}",
              f"-DLLVM_DIR={llvm / 'lib/cmake/llvm'}",
              f"-DClang_DIR={llvm / 'lib/cmake/clang'}",
              source=cli / ".github/audit-extractor")
    build("extractor", "topo-extract-cpp", "topo-cpp-audit-semantic-tests")
    test("extractor", "cpp")
    extractor = work / "extractor/extractor/topo-extract-cpp"
    configure("topo-cli", "-DTOPO_CLI_BUILD_TESTS=ON", "-DTOPO_CLI_REQUIRE_AUDIT_TESTS=ON",
              f"-DPython3_EXECUTABLE={sys.executable}",
              "-DTOPO_CLI_ENABLE_PROFILE=OFF", "-DTOPO_CLI_ENABLE_RUST_PLUGIN=OFF",
              "-DTOPO_CLI_ENABLE_PYTHON_PLUGIN=OFF", "-DTOPO_CLI_ENABLE_TYPESCRIPT_PLUGIN=OFF",
              f"-DTOPO_AUDIT_CPP_EXTRACTOR={extractor}",
              f"-DTOPO_AUDIT_TEST_CLANG={llvm / 'bin/clang++'}",
              f"-DTOPO_AUDIT_JAVA_EXTRACTOR_DIR={prefix / 'bin'}",
              f"-DTOPO_AUDIT_JAVA_BACKEND={prefix / 'bin/topo-build-jvm-java'}",
              f"-DTOPO_GRADLE_JAVA_HOME={java_home}", source=cli)
    build("topo-cli", "topo-check", "topo-build", "topo-audit-support-tests")
    try:
        test("topo-cli", "cli")
    finally:
        for report in (work / "topo-cli").glob("cpp-audit-*.json"):
            shutil.copy2(report, evidence / report.name)
        record_toolchain(evidence, java_home, llvm, extractor, work, prefix)


if __name__ == "__main__":
    main()
