#!/usr/bin/env python3
"""Require topo-init to report an unavailable Java analyzer as a failure."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    init = shutil.which("topo-init")
    if not init:
        raise RuntimeError("topo-init must be installed on PATH")
    environment = os.environ.copy()
    # Keep the compiler's runtime DLL directories on Windows while removing
    # every directory that can provide the optional Java extractor.
    names = ("topo-extract-java", "topo-extract-java.exe", "topo-extract-java.cmd",
             "topo-extract-java.bat")
    environment["PATH"] = os.pathsep.join(
        entry for entry in environment.get("PATH", "").split(os.pathsep)
        if entry and not any((Path(entry) / name).exists() for name in names)
    )
    with tempfile.TemporaryDirectory(prefix="topo-init-analysis-failure-") as temporary:
        project = Path(temporary) / "project"
        source = project / "src/Example.java"
        source.parent.mkdir(parents=True)
        source.write_text("public class Example { public int value() { return 1; } }\n")
        result = subprocess.run([init, "--project", str(project), "--language", "java"],
                                env=environment, capture_output=True, text=True)
        output = result.stdout + result.stderr
        if result.returncode == 0 or "topo-extract-java unavailable" not in output:
            raise RuntimeError(f"Missing analyzer must fail with its diagnostic; exit={result.returncode}\n{output}")
        if (project / "Topo.toml").exists() or (project / "topo").exists():
            raise RuntimeError("Analysis failure must not produce a partial scaffold")
    print("PASS: topo-init rejects unavailable Java analysis and writes no scaffold")


if __name__ == "__main__":
    main()
