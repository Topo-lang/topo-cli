#!/usr/bin/env python3
"""Check header-search closure, native extraction results and current trust."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--check', required=True)
parser.add_argument('--extractor-dir', required=True)
parser.add_argument('--compiler', required=True)
args = parser.parse_args()
env = dict(os.environ)
with tempfile.TemporaryDirectory(prefix='topo-cpp-audit-headers-') as scratch:
    root = Path(scratch).resolve()
    project = root / 'subject'
    project.mkdir()

    def write(name, contents):
        path = project / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents)
        return path

    main = '#include <value.h>\n#if __has_include(<optional.h>)\n#include <optional.h>\n#endif\nnamespace app { void run() { int observed = VALUE + FORCED; (void)observed; } }\n'
    write('main.cpp', main)
    write('first/value.h', '#pragma once\n#if __has_include_next(<value.h>)\n#include_next <value.h>\n#endif\n')
    write('second/value.h', '#pragma once\n#include "detail.h"\n')
    detail = write('second/detail.h', '#define VALUE 7\n')
    write('forced.h', '#define FORCED 1\n')
    (project / 'selected').symlink_to(project / 'second', target_is_directory=True)
    flags = '-std=c++17 -nostdinc -nostdinc++ -Iearly -Ifirst -Iselected -include forced.h'
    write('inner.rsp', flags)
    write('args.rsp', '@inner.rsp -c main.cpp')
    write('compile_commands.json', json.dumps([{'directory': str(project), 'file': 'main.cpp',
          'arguments': [args.compiler, '--no-default-config', '@args.rsp']}]))
    write('main.topo', 'namespace app { public: void run(); }\n')
    write('Topo.toml', '[topo]\nroot="main.topo"\n[build]\nlanguage="cpp"\nsources=["*.cpp"]\n')
    write('Topo.audit.toml', 'version=1\n[[manual]]\nkind="file"\nfile="main.cpp"\n')

    tools = root / 'tools'
    tools.mkdir()
    extractor = Path(args.extractor_dir).resolve() / 'topo-extract-cpp'
    shutil.copy2(extractor, tools / 'topo-extract-cpp')
    env['PATH'] = str(tools) + os.pathsep + env.get('PATH', '')
    cache = root / 'cache'
    env['TOPO_AUDIT_CACHE_DIR'] = str(cache)
    report = root / 'report.json'
    base = [args.check, '--project', str(project), '--trust-store', str(root / 'reviewer/trust.json')]
    runs = 0

    def invoke(extra, expected):
        result = subprocess.run(base + extra, env=env, capture_output=True, text=True, timeout=90)
        assert result.returncode == expected, (extra, result.returncode, result.stdout, result.stderr)

    def audit(status, expected=1, reason=None):
        global runs
        invoke(['--audit', '--audit-report', str(report), '--json'], expected)
        data = json.loads(report.read_text())
        assert data['factsCache']['status'] == status, data['factsCache']
        if reason:
            assert reason in data['factsCache']['reason'], data['factsCache']
        assert data['analysisComplete'], data['unresolved']
        assert data['declarationsPassed'], data
        assert data['manualApproval'] == ('accepted' if expected == 0 else 'pending'), data
        runs += 1
        return data

    cold = audit('miss')
    warm = audit('hit')
    assert cold['reviewKey'] == warm['reviewKey']
    # Corrupt raw facts are discarded and rebuilt, preserving the conclusion.
    entries = list(cache.glob('*.json'))
    assert len(entries) == 1, entries
    entry = json.loads(entries[0].read_text())
    entry['payload']['payload']['complete'] = False
    entries[0].write_text(json.dumps(entry))
    assert audit('miss')['reviewKey'] == cold['reviewKey']
    # A syntactically valid cache entry from an older confinement policy also
    # requires fresh analysis, even when its payload checksum is recomputed.
    entry = json.loads(entries[0].read_text())
    assert isinstance(entry['payload']['execution'], dict)
    entry['payload']['execution']['policy'] = 'obsolete-cpp-analysis-policy'
    canonical = json.dumps(entry['payload'], sort_keys=True, separators=(',', ':'), ensure_ascii=False)
    entry['payloadDigest'] = hashlib.sha256(canonical.encode()).hexdigest()
    entries[0].write_text(json.dumps(entry))
    assert audit('miss')['reviewKey'] == cold['reviewKey']
    invoke(['--accept-review', str(report)], 0)
    assert audit('hit', 0)['reviewKey'] == cold['reviewKey']
    del env['TOPO_AUDIT_CACHE_DIR']
    assert audit('disabled', 0)['reviewKey'] == cold['reviewKey']
    env['TOPO_AUDIT_CACHE_DIR'] = str(cache)
    for entry in cache.iterdir():
        entry.unlink()
    assert audit('miss', 0)['reviewKey'] == cold['reviewKey']
    invoke(['--revoke-review', cold['reviewKey']], 0)
    audit('hit')

    # A changed transitive input invalidates even with the old timestamp.
    previous = detail.stat()
    detail.write_text('#define VALUE 8\n')
    os.utime(detail, ns=(previous.st_atime_ns, previous.st_mtime_ns))
    changed = audit('miss')
    assert changed['reviewKey'] != cold['reviewKey']
    audit('hit')
    write('early/value.h', '#define VALUE 20\n')
    changed = audit('miss')
    audit('hit')
    write('early/optional.h', '#define OPTIONAL 100\n')
    assert audit('miss')['reviewKey'] != changed['reviewKey']
    audit('hit')
    write('forced.h', '#define FORCED 2\n')
    audit('miss')
    write('inner.rsp', flags.replace('-Iearly -Ifirst -Iselected', '-Iselected -Iearly -Ifirst'))
    audit('miss')
    write('third/value.h', '#define VALUE 30\n')
    (project / 'selected').unlink()
    (project / 'selected').symlink_to(project / 'third', target_is_directory=True)
    audit('miss')
    audit('hit')

    # Dynamic preprocessing remains fresh even when hidden by token pasting.
    write('third/value.h', '#define VALUE 30\n#define JOIN(a,b) a ## b\n')
    write('main.cpp', main.replace('int observed', 'const char* clock = JOIN(__TI, ME__); int observed'))
    audit('bypass', reason='token paste')
    audit('bypass', reason='token paste')
    write('third/value.h', '#define VALUE 30\n#pragma clang system_header\n')
    write('main.cpp', main)
    audit('bypass', reason='pragma')

    print(f'C++ audit header closure: {runs} scenarios passed; 0 skipped')
