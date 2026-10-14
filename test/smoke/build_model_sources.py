#!/usr/bin/env python3
"""Exercise public build-model source validation for directory-based backends."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(command, expected=0, diagnostic=""):
    result = subprocess.run([str(arg) for arg in command], capture_output=True, text=True)
    output = result.stdout + result.stderr
    if (result.returncode == 0) != (expected == 0) or diagnostic not in output:
        raise RuntimeError(f"Unexpected exit {result.returncode} for {command}\n{output}")


def main():
    init, check = shutil.which("topo-init"), shutil.which("topo-check")
    if not init or not check:
        raise RuntimeError("topo-init and topo-check must be installed on PATH")
    fixtures = Path(__file__).resolve().parent / "fixtures"
    passed = 0
    with tempfile.TemporaryDirectory(prefix="topo-build-model-sources-") as temporary:
        for language in ("python", "typescript", "cpp", "java"):
            project = Path(temporary) / (language + "smoke")
            shutil.copytree(fixtures / language, project)
            run([init, "--project", project, "--language", language])
            model = {"schemaVersion": 1, "language": language,
                     "topoRoot": str(project / "topo/main.topo"), "sources": ["src"],
                     "includeDirs": []}
            path = project / "build-model.json"
            path.write_text(json.dumps(model))
            command = [check, "--project", project, "--build-model", path]
            if language in ("python", "typescript"):
                run(command)
                passed += 1
                model["sources"] = ["missing-source-directory"]
                path.write_text(json.dumps(model))
                run(command, expected=1, diagnostic="invalid resolved build model")
            else:
                run(command, expected=1, diagnostic="build source is not a file")
            passed += 1
    print(f"PASS: {passed} build-model source scenarios; source directories stay language-specific")


if __name__ == "__main__":
    main()
