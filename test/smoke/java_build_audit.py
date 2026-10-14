#!/usr/bin/env python3
"""Exercise real Java build approval and reject changed compiler inputs.

Requires JDK 25, the Java extractor, and runtime/transform JARs installed next
to topo-build-jvm-java. Invocations compile and inspect a fixture JAR; they
never execute the fixture's classes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile


parser = argparse.ArgumentParser()
parser.add_argument('--build', required=True)
parser.add_argument('--check', required=True)
parser.add_argument('--backend', required=True)
parser.add_argument('--extractor-dir', required=True)
parser.add_argument('--java-home', required=True)
args = parser.parse_args()
build = str(Path(args.build).resolve(strict=True))
check = str(Path(args.check).resolve(strict=True))
backend = str(Path(args.backend).resolve(strict=True))
java_home = Path(args.java_home).resolve(strict=True)
env = dict(os.environ, JAVA_HOME=str(java_home))
env['PATH'] = os.pathsep.join([
    str(java_home / 'bin'), str(Path(args.extractor_dir).resolve(strict=True)),
    str(Path(backend).parent), str(Path(check).parent),
    os.environ.get('PATH', ''),
])


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


with tempfile.TemporaryDirectory(prefix='topo-java-build-audit-') as scratch:
    root = Path(scratch).resolve()
    project = root / 'subject'
    (project / 'app').mkdir(parents=True)
    source = project / 'app/Log.java'
    original = ('package app; public final class Log { '
                'private static void hidden(){System.out.println("reviewed");} '
                'public static void write(){hidden();} }\n')
    source.write_text(original)
    (project / 'main.topo').write_text(
        'namespace app { public: external type Log {public: static void write();}}\n')
    (project / 'Topo.toml').write_text(
        '[topo]\nroot="main.topo"\n[build]\nlanguage="java"\n'
        'sources=["app/Log.java"]\n[build.java]\ntarget_version="25"\n'
        'java_home=' + json.dumps(str(java_home)) + '\n[pipeline]\nmode="off"\n')
    report = root / 'report.json'
    trust = root / 'reviewer/trust.json'
    artifact = project / 'app.jar'
    evidence = root / 'evidence.json'
    command = [build, '--audit', '--audit-report', str(report),
               '--trust-store', str(trust), '--evidence', 'commands',
               '--evidence-output', str(evidence), '-o', str(artifact)]
    check_command = [check, '--project', str(project), '--trust-store', str(trust)]
    scenarios = []

    def invoke(name, argv, expected):
        result = subprocess.run(argv, cwd=project, env=env, capture_output=True,
                                text=True, timeout=180)
        assert result.returncode == expected, (
            name, result.returncode, result.stdout, result.stderr)
        scenarios.append(name)
        return result

    def read_report():
        return json.loads(report.read_text())

    def verify_jar():
        with zipfile.ZipFile(artifact) as jar:
            assert 'app/Log.class' in jar.namelist()

    invoke('external-runtime-denied', command, 1)
    denied = read_report()
    assert not denied['analysisComplete'] and 'analysis-input-root' in str(denied), denied
    assert not artifact.exists()
    command += ['--analysis-input-root', str(Path(backend).parent)]
    invoke('pending', command, 1)
    first = read_report()
    assert first['analysisComplete'] and first['manualApproval'] == 'pending', first
    assert [entry['name'] for entry in first['manual']] == ['app::Log']
    assert not artifact.exists()
    invoke('accept', check_command + ['--accept-review', str(report)], 0)
    invoke('approved-build', command, 0)
    assert read_report()['passed'] and read_report()['reviewKey'] == first['reviewKey']
    verify_jar()
    manifest = json.loads(evidence.read_text())
    assert manifest['nodes'], 'Actual build must emit command evidence'
    invoke('repeat-build', command, 0)
    assert read_report()['reviewKey'] == first['reviewKey']
    verify_jar()

    requests = list((project / '.topo-cache/audit-build').glob('*/backend-request.json'))
    assert len(requests) == 1, requests
    request_path = requests[0]

    def reject_backend(name, diagnostic):
        before = digest(artifact)
        stages = set(request_path.parent.glob('jvm-stage-*'))
        result = invoke(name, [backend, str(request_path)], 1)
        assert diagnostic in result.stderr, result.stderr
        assert digest(artifact) == before, 'Rejected inputs changed the existing JAR'
        assert set(request_path.parent.glob('jvm-stage-*')) == stages, (
            'Rejected inputs reached a new compiler stage')

    source.write_text(original.replace('reviewed', 'changed'))
    reject_backend('changed-source', 'checked Java project model differs')
    invoke('stale-review', check_command + ['--accept-review', str(report)], 2)
    invoke('changed-source-pending', command, 1)
    changed = read_report()
    assert changed['manualApproval'] == 'pending'
    assert changed['reviewKey'] != first['reviewKey']
    source.write_text(original)
    invoke('restored-source-build', command, 0)
    assert read_report()['passed'] and read_report()['reviewKey'] == first['reviewKey']
    verify_jar()

    request_bytes = request_path.read_bytes()
    request = json.loads(request_bytes)
    model = Path(request['checkModelPath'])
    model_bytes = model.read_bytes()
    model.write_bytes(model_bytes + b'\n')
    reject_backend('tampered-model', 'checked Java project model digest does not match')
    model.write_bytes(model_bytes)
    request.setdefault('backendExtras', {})['jvmArgs'] = ['-Dtopo.audit.changed=true']
    request_path.write_text(json.dumps(request))
    reject_backend('tampered-recipe', 'checked Java project model differs')
    request_path.write_bytes(request_bytes)

assert len(scenarios) == 11, scenarios
print('Java build audit: 11 scenarios passed; 0 skipped')
