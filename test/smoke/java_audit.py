#!/usr/bin/env python3
"""Exercise the actual Java audit CLI and protected approval lifecycle."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--check', required=True)
parser.add_argument('--extractor-dir', required=True)
args = parser.parse_args()
env = dict(os.environ, PATH=args.extractor_dir + os.pathsep + os.environ.get('PATH', ''))
with tempfile.TemporaryDirectory(prefix='topo-java-audit-') as scratch:
    root = Path(scratch)
    project = root / 'subject'
    project.mkdir()
    (project / 'app').mkdir()
    source = project / 'app/Log.java'
    original = 'package app; public final class Log { private static void hidden(){ System.out.println("reviewed"); } public static void write(){ hidden(); } }\n'
    source.write_text(original)
    (project / 'main.topo').write_text('namespace app { public: external type Log { public: static void write(); } }\n')
    config = '[topo]\nroot="main.topo"\n[build]\nlanguage="java"\nsources=["app/Log.java"]\n[check]\njava_project="java-project.json"\n'
    (project / 'Topo.toml').write_text(config)
    (project / 'java-project.json').write_text(json.dumps({'sourceLevel':'25','files':['app/Log.java'],'sourcepath':['.'],'classpath':[]}))
    report = root / 'report.json'
    trust = root / 'user/trust.json'
    base = [args.check, '--project', str(project), '--trust-store', str(trust)]
    def invoke(extra, expected):
        result = subprocess.run(base + extra, capture_output=True, text=True, env=env, timeout=90)
        assert result.returncode == expected, (extra, result.returncode, result.stdout, result.stderr)
        return result
    def audit(expected):
        invoke(['--audit', '--audit-report', str(report), '--json'], expected)
        return json.loads(report.read_text())
    first = audit(1)
    assert first['analysisComplete'] and first['manualApproval']=='pending'
    assert [entry['name'] for entry in first['manual']]==['app::Log']
    invoke(['--accept-review',str(report)],0)
    assert audit(0)['passed']
    invoke(['--revoke-review',first['reviewKey']],0)
    assert audit(1)['manualApproval']=='pending'
    invoke(['--accept-review',str(report)],0)
    source.write_text(original.replace('reviewed','changed'))
    invoke(['--accept-review',str(report)],2)
    assert audit(1)['manualApproval']=='pending'
    source.write_text(original)
    assert audit(0)['passed']
    source.write_text(original+'class Other { static void hidden(){ System.out.println("unreviewed"); } }\n')
    different = audit(1)
    assert all('Other' not in str(entry) for entry in different['manual'])
    invoke(['--accept-review',str(report)],0)
    assert not audit(1)['declarationsPassed']
    source.write_text(original)
    invoke(['--audit','--deep'],2)
    (project/'Topo.toml').write_text(config+'[containment]\nmode="off"\n')
    invoke(['--audit'],2)
    (project/'Topo.toml').write_text(config)
    invoke(['--audit','--trust-store',str(project/'trust.json')],2)
    (project/'Topo.audit.toml').write_text('strict_dependencies="yes"\n')
    invoke(['--audit'],2)
print('Java audit lifecycle: 12 scenarios passed; 0 skipped')
