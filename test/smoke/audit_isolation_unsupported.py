#!/usr/bin/env python3
"""Unsupported platforms reject auditing before launching a supplied tool."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import shutil
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--check', required=True)
parser.add_argument('--language', choices=('cpp', 'java'), required=True)
parser.add_argument('--sentinel-executable', required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='topo-audit-unsupported-') as temporary:
    root = Path(temporary).resolve()
    project = root / 'project'
    tools = root / 'tools'
    project.mkdir()
    tools.mkdir()
    sentinel = tools / 'unexpected-execution'
    suffix = '.exe' if os.name == 'nt' else ''
    for name in ('topo-extract-cpp', 'topo-extract-java', 'clang++', 'java', 'javac'):
        shutil.copy2(args.sentinel_executable, tools / (name + suffix))
    (project / 'main.topo').write_text('namespace app { public: void run(); }\n')
    (project / 'Topo.toml').write_text(f'[topo]\nroot="main.topo"\n[build]\nlanguage="{args.language}"\n')
    if args.language == 'cpp':
        (project / 'main.cpp').write_text('namespace app { void run() {} }\n')
        (project / 'compile_commands.json').write_text(json.dumps([{
            'directory': str(project), 'file': 'main.cpp',
            'arguments': [str(tools / ('clang++' + suffix)), '--no-default-config', '-c', 'main.cpp']}]))
    else:
        (project / 'Main.java').write_text('class Main { public static void run() {} }\n')
    env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ.get('PATH', ''))
    # Confirm that the native marker works even when launch environment is cleared.
    control = subprocess.run([str(tools / ('java' + suffix))], env={}, cwd=project, timeout=10)
    assert control.returncode == 86 and sentinel.exists(), 'native execution marker is ineffective'
    sentinel.unlink()
    result = subprocess.run([args.check, '--project', str(project), '--audit', '--json'],
                            env=env, cwd=project, capture_output=True, text=True, timeout=30)
    assert result.returncode == 2, (result.returncode, result.stdout, result.stderr)
    assert 'audit execution unsupported' in result.stderr, result.stderr
    assert not sentinel.exists(), 'an analysis tool ran despite unsupported isolation'
print(f'{args.language} unsupported audit isolation: 1 scenario passed; 0 skipped')
