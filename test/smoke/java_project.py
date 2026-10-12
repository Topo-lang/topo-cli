#!/usr/bin/env python3
"""Exercise real CLI/project-model wiring with javac-built external dependencies."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--check', required=True)
p.add_argument('--transpile', required=True)
p.add_argument('--javac', required=True)
p.add_argument('--extractor-dir', required=True)
a = p.parse_args()
env = dict(os.environ, PATH=a.extractor_dir + os.pathsep + os.environ.get('PATH', ''))

def run(args, expected=0, command_env=None):
    result = subprocess.run([str(v) for v in args], env=command_env or env, text=True, capture_output=True, timeout=90)
    assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
    return result

with tempfile.TemporaryDirectory(prefix='topo-java-project-') as tmp:
    root = Path(tmp)
    (root/'src/app').mkdir(parents=True)
    (root/'dep/external').mkdir(parents=True)
    (root/'classes').mkdir()
    (root/'model').mkdir()
    (root/'dep/external/Base.java').write_text('package external; public class Base {}\n')
    run([a.javac, '--release', '25', '-d', root/'classes', root/'dep/external/Base.java'])
    (root/'src/app/Calculator.java').write_text('''package app;
import external.Base;
public class Calculator {
 public static int add(int a, int b) { return a + b; }
}
''')
    (root/'api.topo').write_text('namespace app { public: type Calculator { public: integer add(integer a, integer b); } }\n')
    (root/'Topo.toml').write_text('''[topo]
root = "api.topo"
[build]
language = "java"
sources = ["src"]
[completeness]
ignore_constructors = true
[check]
java_project = "model/project.json"
''')
    model = {'sourceLevel':'25', 'classpath':['../classes'], 'sourcepath':['../src'], 'files':['../src/app/Calculator.java']}
    manifest = root/'model/project.json'
    manifest.write_text(json.dumps(model))
    result = run([a.check, '--project', root, '--json'])
    assert json.loads(result.stdout)['passed'] is True
    # Explicit CLI model resolves relative to the model, including invocation
    # from a working directory outside the Java project.
    run([a.check, '--project', root, '--java-project', manifest, '--check', 'completeness'])
    run([a.transpile, '--from', 'java', '--to', 'python', '--java-project', manifest,
         '--functions', 'app::Calculator::add', '--verify-strict', '--output', root/'out', root/'api.topo'])
    outputs = list((root/'out').glob('*.py'))
    assert outputs, 'no generated source'
    namespace = {}
    exec(compile(outputs[0].read_text(), str(outputs[0]), 'exec'), namespace)
    assert namespace['Calculator'].add(2, 3) == 5, 'generated method behavior changed'
    # An explicit source selection replaces even stale model file entries.
    stale = root/'model/stale.json'
    stale.write_text(json.dumps(dict(model, files=['../src/app/Removed.java'])))
    run([a.transpile, '--from', 'java', '--to', 'python', '--java-project', stale,
         '--sources', root/'src/app/Calculator.java', '--verify-strict',
         '--output', root/'override', root/'api.topo'])
    # Dependency removal must invalidate the previous successful result even
    # though both Java source and model JSON are unchanged.
    (root/'classes/external/Base.class').unlink()
    bad = run([a.check, '--project', root, '--json'], expected=1)
    report = json.loads(bad.stdout)
    assert report['passed'] is False
    assert any(c['name'] == 'java-analysis' and c['errorCount'] > 0 for c in report['checks'])
    malformed = root/'model/bad.json'
    malformed.write_text('{"classpath": "wrong"}')
    run([a.transpile, '--from', 'java', '--to', 'python', '--java-project', malformed, root/'api.topo'], expected=1)
    # A stale or incomplete extractor must fail before the portable module can
    # be mistaken for successfully analyzed Java source.
    fake = root/'legacy-extractor'
    fake.mkdir()
    response_file = fake/'response.json'
    responder = fake/'respond.py'
    responder.write_text('import pathlib, sys\nsys.stdin.read()\nsys.stdout.write(pathlib.Path(__file__).with_name("response.json").read_text())\n')
    if os.name == 'nt':
        (fake/'topo-extract-java.cmd').write_text('@echo off\n' + subprocess.list2cmdline([sys.executable, str(responder)]) + '\n')
    else:
        launcher = fake/'topo-extract-java'
        launcher.write_text('#!/bin/sh\nexec ' + shlex.join([sys.executable, str(responder)]) + '\n')
        launcher.chmod(0o755)
    fake_env = dict(env, PATH=str(fake) + os.pathsep + env['PATH'])
    for response in [
        {'types': [], 'functions': []},
        {'schemaVersion': 2, 'types': [], 'functions': [], 'diagnostics': []},
        {'schemaVersion': 2, 'types': [], 'functions': [], 'analysisComplete': True},
        {'schemaVersion': 2, 'types': [], 'functions': [], 'analysisComplete': False,
         'diagnostics': [{'severity': 'warning', 'message': 'incomplete binding'}]},
    ]:
        response_file.write_text(json.dumps(response))
        rejected = run([a.transpile, '--from', 'java', '--to', 'python',
                        '--sources', root/'src/app/Calculator.java', '--verify-strict',
                        '--output', root/'rejected', root/'api.topo'], expected=1, command_env=fake_env)
        assert 'schemaVersion 2' in rejected.stderr or 'incomplete analysis' in rejected.stderr
        assert not (root/'rejected').exists(), 'invalid analysis created output'
print('PASS: Java project model CLI, generated Python execution, source override, dependency invalidation, malformed input, extractor protocol validation')
