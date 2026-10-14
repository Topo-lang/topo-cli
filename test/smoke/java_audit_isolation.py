#!/usr/bin/env python3
"""Actual JVM startup-agent control and isolated JDT audit boundaries."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--check', required=True)
parser.add_argument('--extractor-dir', required=True)
parser.add_argument('--java-home', required=True)
args = parser.parse_args()
java_home = Path(args.java_home).resolve()
base_env = dict(os.environ, PATH=args.extractor_dir + os.pathsep + os.environ.get('PATH', ''))
for name in ('JAVA_TOOL_OPTIONS', 'JDK_JAVA_OPTIONS', '_JAVA_OPTIONS'):
    base_env.pop(name, None)
with tempfile.TemporaryDirectory(prefix='topo-java-isolation-') as temporary:
    root = Path(temporary).resolve()
    agent = root / 'StartupAgent.java'
    agent.write_text('import java.nio.file.*; public class StartupAgent { public static void premain(String path) throws Exception { Files.writeString(Path.of(path), "executed"); } }')
    manifest = root / 'MANIFEST.MF'
    manifest.write_text('Premain-Class: StartupAgent\n')
    subprocess.run([str(java_home / 'bin/javac'), str(agent)], check=True, env=base_env, capture_output=True)
    agent_jar = root / 'startup-agent.jar'
    subprocess.run([str(java_home / 'bin/jar'), 'cfm', str(agent_jar), str(manifest), '-C', str(root), 'StartupAgent.class'], check=True, env=base_env, capture_output=True)
    sentinel = root / 'startup-sentinel'
    injected = dict(base_env, JAVA_TOOL_OPTIONS=f'-javaagent:{agent_jar}={sentinel}')
    subprocess.run([str(java_home / 'bin/java'), '-version'], check=True, env=injected, capture_output=True)
    assert sentinel.read_text() == 'executed', 'startup agent positive control must execute'
    sentinel.unlink()
    injected['JDK_JAVA_OPTIONS'] = injected['JAVA_TOOL_OPTIONS']
    injected['_JAVA_OPTIONS'] = injected['JAVA_TOOL_OPTIONS']
    project = root / 'subject'
    project.mkdir()
    (project / 'app').mkdir()
    (project / 'app/Log.java').write_text('package app; public final class Log { public static void write() {} }\n')
    (project / 'main.topo').write_text('namespace app { public: external type Log { public: static void write(); } }\n')
    (project / 'Topo.toml').write_text('[topo]\nroot="main.topo"\n[build]\nlanguage="java"\nsources=["app/Log.java"]\n[check]\njava_project="java-project.json"\n')
    model = project / 'java-project.json'
    model.write_text(json.dumps({'sourceLevel': '25', 'files': ['app/Log.java']}))
    report = root / 'report.json'
    command = [args.check, '--project', str(project), '--audit', '--audit-report', str(report), '--trust-store', str(root / 'reviewer/trust.json'), '--json']
    def audit(extra=()):
        result = subprocess.run(command + list(extra), env=injected, capture_output=True, text=True, timeout=180)
        assert result.returncode == 1, (result.returncode, result.stdout, result.stderr)
        assert not sentinel.exists(), 'audit must clear JVM startup-agent variables before spawn'
        return json.loads(report.read_text())
    first = audit()
    assert first['analysisComplete'], first.get('unresolved')
    assert first['analysisIsolation'] == 'macos-seatbelt', first
    assert any(Path(entry['path']).name == 'topo-analysis-launcher' for entry in first['inputs']), 'bind the native isolation launcher identity'
    # A project model cannot grant reads to an external dependency directory.
    dependency = root / 'external-inputs'
    dependency.mkdir()
    model.write_text(json.dumps({'sourceLevel': '25', 'files': ['app/Log.java'], 'classpath': [str(dependency)], 'dependencies': [{'path': str(dependency), 'kind': 'external', 'coordinate': 'fixture:inputs', 'version': '1'}]}))
    denied = audit()
    assert not denied['analysisComplete'], denied
    assert 'analysis-input-root' in json.dumps(denied), denied
    allowed = audit(['--analysis-input-root', str(dependency)])
    assert allowed['analysisComplete'], allowed.get('unresolved')

    def assert_unpublished(data, path):
        assert not data['analysisComplete'], data
        assert str(path) not in data['sourceFiles'], data
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        assert digest not in json.dumps(data), 'rejected input must not be hashed into the report'
        assert all(item['path'] != str(path) and item['resolvedPath'] != str(path)
                   for item in data['inputs']), data

    external_model = dependency / 'external-model.json'
    external_model.write_text('SYNTHETIC_UNAUTHORIZED_MODEL_BYTES')
    denied_model = audit(['--java-project', str(external_model)])
    assert_unpublished(denied_model, external_model)
    assert 'SYNTHETIC_UNAUTHORIZED_MODEL_BYTES' not in json.dumps(denied_model), denied_model
    linked_model = project / 'linked-model.json'
    linked_model.symlink_to(external_model)
    denied_link = audit(['--java-project', str(linked_model)])
    assert_unpublished(denied_link, external_model)
    assert 'SYNTHETIC_UNAUTHORIZED_MODEL_BYTES' not in json.dumps(denied_link), denied_link

    external_source = dependency / 'External.java'
    external_source.write_text('class External {} // synthetic source outside approved roots\n')
    model.write_text(json.dumps({'sourceLevel': '25', 'files': [str(external_source)]}))
    denied_source = audit()
    assert_unpublished(denied_source, external_source)
    assert not denied_source['sourceFiles'], denied_source

    model.write_text(json.dumps({'sourceLevel': '25', 'files': []}))
    empty_selection = audit()
    assert not empty_selection['sourceFiles'], empty_selection
    assert not empty_selection['analysisComplete'], empty_selection

    external_model.write_text(json.dumps({'sourceLevel': '25', 'files': [str(project / 'app/Log.java')]}))
    allowed_model = audit(['--java-project', str(external_model), '--analysis-input-root', str(dependency)])
    assert allowed_model['analysisComplete'], allowed_model.get('unresolved')
    assert any(item['path'] == str(external_model) for item in allowed_model['inputs']), allowed_model
print('Java isolation and parent inputs: 10 scenarios passed; 0 skipped')
