#!/usr/bin/env python3
"""Verify manual-file approval never authorizes a same-named sibling body."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import shutil
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--check', required=True)
parser.add_argument('--extractor-dir', required=True)
parser.add_argument('--compiler', required=True)
args = parser.parse_args()
env = dict(os.environ, PATH=args.extractor_dir + os.pathsep + os.environ.get('PATH', ''))
with tempfile.TemporaryDirectory(prefix='topo-cpp-audit-') as scratch:
    root = Path(scratch).resolve()
    project = root / 'subject'
    project.mkdir()
    (project / 'reviewed.cpp').write_text('namespace app { static void helper() {} }\n')
    other = project / 'unreviewed.cpp'
    other.write_text('namespace app { static void helper() { asm("nop"); } }\n')
    (project / 'main.topo').write_text('namespace app { public: void helper(); }\n')
    (project / 'Topo.toml').write_text('[topo]\nroot="main.topo"\n[build]\nlanguage="cpp"\nsources=["*.cpp"]\n')
    (project / 'Topo.audit.toml').write_text('version=1\n[[manual]]\nkind="file"\nfile="reviewed.cpp"\n')
    (project / 'compile_commands.json').write_text(json.dumps([
        {'directory': str(project), 'file': name,
         'arguments': [args.compiler, '--no-default-config', '-std=c++17', '-c', name]}
        for name in ('reviewed.cpp', 'unreviewed.cpp')]))
    report = root / 'report.json'
    base = [args.check, '--project', str(project), '--trust-store', str(root / 'user/trust.json')]
    def invoke(extra, expected):
        result = subprocess.run(base + extra, env=env, capture_output=True, text=True, timeout=90)
        assert result.returncode == expected, (extra, result.returncode, result.stdout, result.stderr)
        return result
    def audit(expected):
        invoke(['--audit', '--audit-report', str(report), '--json'], expected)
        return json.loads(report.read_text())
    first = audit(1)
    assert first['analysisComplete'] and first['manualApproval'] == 'pending', first
    invoke(['--accept-review', str(report)], 0)
    refused = audit(1)
    assert refused['manualApproval'] == 'accepted'
    assert not refused['declarationsPassed'] and not refused['passed']
    other.write_text('namespace app { static void helper() {} }\n')
    assert audit(1)['manualApproval'] == 'pending'
    invoke(['--accept-review', str(report)], 0)
    assert audit(0)['passed']

    # Exercise installed native extractors outside the subject project. Cache
    # labels are checked as output; this smoke does not independently count
    # semantic subprocesses inside the sandbox.
    tool_dir = root / 'tools'
    tool_dir.mkdir()
    real_tool = Path(args.extractor_dir).resolve() / 'topo-extract-cpp'
    shutil.copy2(real_tool, tool_dir / 'topo-extract-cpp')
    env['PATH'] = str(tool_dir) + os.pathsep + env['PATH']
    cache = root / 'reviewer-cache'
    env['TOPO_AUDIT_CACHE_DIR'] = str(cache)
    model = project / 'compile_commands.json'
    commands = json.loads(model.read_text())
    for command in commands:
        command['arguments'][2:2] = ['-nostdinc', '-nostdinc++']
    model.write_text(json.dumps(commands))
    def cached_audit(expected, status):
        started = time.perf_counter()
        data = audit(expected)
        assert data['factsCache']['status'] == status, data['factsCache']
        timings.append((status, time.perf_counter() - started))
        return data

    timings = []
    cold = cached_audit(1, 'miss')
    base.extend(['--audit-cache', str(cache)])
    warm = cached_audit(1, 'hit')
    del base[-2:]
    assert warm['reviewKey'] == cold['reviewKey']
    invoke(['--accept-review', str(report)], 0)
    assert cached_audit(0, 'hit')['passed']
    del env['TOPO_AUDIT_CACHE_DIR']
    disabled = cached_audit(0, 'disabled')
    assert disabled['reviewKey'] == cold['reviewKey']
    env['TOPO_AUDIT_CACHE_DIR'] = str(cache)
    invoke(['--revoke-review', warm['reviewKey']], 0)
    assert cached_audit(1, 'hit')['manualApproval'] == 'pending'
    for entry in cache.iterdir():
        entry.unlink()
    refreshed = cached_audit(1, 'miss')
    assert refreshed['reviewKey'] == cold['reviewKey']

    before = other.stat()
    other.write_text('namespace app { static void helper() { asm("nop"); } }\n')
    os.utime(other, ns=(before.st_atime_ns, before.st_mtime_ns))
    changed = cached_audit(1, 'miss')
    assert changed['reviewKey'] != refreshed['reviewKey']
    assert not changed['declarationsPassed']
    invoke(['--accept-review', str(report)], 0)
    assert not cached_audit(1, 'hit')['passed']

    # An in-project cache and a forged ordinary PASS never supply approvals.
    (project / '.topo-check-cache').write_text(json.dumps({'version': 999, 'result': 0}))
    env['TOPO_AUDIT_CACHE_DIR'] = str(project / 'cache')
    denied = cached_audit(1, 'miss')
    assert 'outside' in denied['factsCache']['reason']
    env['TOPO_AUDIT_CACHE_DIR'] = str(cache)

    replacement_dir = root / 'replacement-tools'
    replacement_dir.mkdir()
    shutil.copy2(real_tool, replacement_dir / 'topo-extract-cpp')
    env['PATH'] = str(replacement_dir) + os.pathsep + env['PATH']
    new_tool = cached_audit(1, 'miss')
    assert new_tool['reviewKey'] != changed['reviewKey']
    assert new_tool['manualApproval'] == 'pending'
    policy = project / 'Topo.audit.toml'
    policy.write_text(policy.read_text() + '# reviewed policy revision\n')
    new_policy = cached_audit(1, 'miss')
    assert new_policy['reviewKey'] != new_tool['reviewKey']

    # Translation-phase splicing must not hide a time-dependent builtin from
    # reuse eligibility, even though ordinary semantic extraction accepts it.
    other.write_text('namespace app { static void helper() { const char* clock = __TI\\\nME__; } }\n')
    assert 'preprocessor' in cached_audit(1, 'bypass')['factsCache']['reason']
    cached_audit(1, 'bypass')

    # Literal project headers now participate in the cache input closure.
    (project / 'header.h').write_text('// empty header\n')
    other.write_text('#include "header.h"\nnamespace app { static void helper() {} }\n')
    supported = cached_audit(1, 'miss')
    assert supported['analysisComplete']
    assert cached_audit(1, 'hit')['reviewKey'] == supported['reviewKey']
    first_times = {status: next(seconds for name, seconds in timings if name == status)
                   for status in ('miss', 'hit', 'disabled')}
    print('Same-run cache timing (seconds): ' + json.dumps(first_times, sort_keys=True))
print('C++ audit physical-owner/cache lifecycle: 19 scenarios passed; 0 skipped')
