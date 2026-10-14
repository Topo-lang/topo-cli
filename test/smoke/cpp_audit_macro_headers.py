#!/usr/bin/env python3
"""Check finite object-macro header closure against real isolated extraction."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--check', required=True)
parser.add_argument('--extractor-dir', required=True)
parser.add_argument('--compiler', required=True)
parser.add_argument('--evidence', required=True)
args = parser.parse_args()
evidence = {'scenarios': [], 'passed': 0, 'failed': 0, 'skipped': 0}
env = dict(os.environ)

try:
    with tempfile.TemporaryDirectory(prefix='topo-cpp-audit-macro-headers-') as scratch:
        root = Path(scratch).resolve()
        project = root / 'subject'
        project.mkdir()

        def write(name, contents):
            path = project / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents)
            return path

        body = 'namespace app { void run() { int observed = VALUE + BONUS; (void)observed; } }\n'
        main = '#include HEADER\n#ifndef BONUS\n#define BONUS 0\n#endif\n#if __has_include(OPTIONAL)\n#include OPTIONAL\n#endif\n' + body
        definitions = '#define HEADER ALIAS\n#if CHOICE\n#define ALIAS "value.h"\n#else\n#define ALIAS "other.h"\n#endif\n#define OPTIONAL "optional.h"\n'
        write('main.cpp', main)
        write('macros.h', definitions)
        write('late/value.h', '#define DETAIL "detail.h"\n#include DETAIL\n')
        detail = write('late/detail.h', '#define VALUE 7\n')
        write('late/other.h', '#define VALUE 20\n')
        (project / 'early').mkdir()
        flags = ['--no-default-config', '-std=c++17', '-nostdinc', '-nostdinc++',
                 '-Iearly', '-Ilate', '-imacros', 'macros.h', '-DCHOICE=1', '-c', 'main.cpp']

        def model():
            write('compile_commands.json', json.dumps([{'directory': str(project), 'file': 'main.cpp',
                  'arguments': [args.compiler] + flags}]))

        model()
        write('main.topo', 'namespace app { public: void run(); }\n')
        write('Topo.toml', '[topo]\nroot="main.topo"\n[build]\nlanguage="cpp"\nsources=["*.cpp"]\n')
        write('Topo.audit.toml', 'version=1\n[[manual]]\nkind="file"\nfile="main.cpp"\n')
        tools = root / 'tools'
        tools.mkdir()
        extractor = Path(args.extractor_dir).resolve() / 'topo-extract-cpp'
        shutil.copy2(extractor, tools / extractor.name)
        env['PATH'] = str(tools) + os.pathsep + env.get('PATH', '')
        env['TOPO_AUDIT_CACHE_DIR'] = str(root / 'cache')
        report = root / 'report.json'
        base = [args.check, '--project', str(project), '--trust-store', str(root / 'reviewer/trust.json')]

        def audit(name, status, complete=True, reason=None, extra=()):
            command = base + ['--audit', '--audit-report', str(report), '--json'] + list(extra)
            result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=90)
            data = json.loads(report.read_text()) if report.exists() else None
            observed = {'name': name, 'command': command, 'exitCode': result.returncode,
                        'stdout': result.stdout, 'stderr': result.stderr, 'report': data,
                        'files': {str(p.relative_to(project)): {'sha256': hashlib.sha256(p.read_bytes()).hexdigest(),
                                   'text': p.read_text()} for p in project.rglob('*') if p.is_file() and not p.is_symlink()}}
            evidence['scenarios'].append(observed)
            assert result.returncode == 1, observed
            assert data['factsCache']['status'] == status, observed
            assert data['analysisComplete'] == complete, observed
            if complete:
                assert data['declarationsPassed'], observed
                assert data['manualApproval'] == 'pending', observed
            if reason:
                assert reason in data['factsCache']['reason'], observed
            evidence['passed'] += 1
            return data

        cold = audit('quoted-alias-cold', 'miss')
        assert audit('quoted-alias-warm', 'hit')['reviewKey'] == cold['reviewKey']
        previous = detail.stat()
        detail.write_text('#define VALUE 8\n')
        os.utime(detail, ns=(previous.st_atime_ns, previous.st_mtime_ns))
        changed = audit('transitive-bytes-same-mtime', 'miss')
        assert changed['reviewKey'] != cold['reviewKey']
        audit('transitive-warm', 'hit')
        write('early/value.h', '#define VALUE 9\n')
        audit('higher-priority-header-created', 'miss')
        audit('higher-priority-warm', 'hit')
        write('early/optional.h', '#undef BONUS\n#define BONUS 100\n')
        audit('negative-macro-probe-became-present', 'miss')
        audit('probe-warm', 'hit')
        flags += ['-UCHOICE', '-D', 'CHOICE=0']
        model()
        audit('ordered-command-macro-branch', 'miss')
        audit('command-branch-warm', 'hit')
        write('macros.h', '#define HEADER ALIAS\n#define ALIAS "value.h"\n#define OPTIONAL "optional.h"\n')
        audit('alias-definition-changed', 'miss')
        audit('alias-definition-warm', 'hit')
        (project / 'early/value.h').unlink()
        audit('shadowing-header-removed', 'miss')

        # Each unsupported form is valid in this translation unit; bypass means
        # fresh semantic extraction still succeeds rather than hiding an error.
        for name, source, reason in [
            ('function-header', '#define H(x) x\n#include H("value.h")\n', 'complex'),
            ('angle-header', '#define H <value.h>\n#include H\n', 'angle-token'),
            ('inactive-cycle', '#define A B\n#define B A\n#if 0\n#include A\n#endif\n#include "value.h"\n', 'cyclic'),
            ('inactive-undefined', '#if 0\n#include UNKNOWN\n#endif\n#include "value.h"\n', 'undefined'),
            ('header-undef', '#define H "other.h"\n#undef H\n#define H "value.h"\n#include H\n', 'complex'),
        ]:
            write('main.cpp', source + '#define BONUS 0\n' + body)
            audit(name, 'bypass', reason=reason)

        for probe in ('__has_include', '__has_include_next'):
            write('main.cpp', '#include "value.h"\nnamespace app { void run(){\n#undef ' + probe + '\n("value.h");\n} }\n')
            audit('builtin-undef-followed-by-expression-' + probe, 'bypass', reason='overridden include probe')

        # Expanded spellings remain subject to the same explicit-root policy.
        external = root / 'external'
        external.mkdir()
        private_header = external / 'private.h'
        private_header.write_text('#define VALUE 33\n')
        write('main.cpp', '#define H "private.h"\n#include H\n#define BONUS 0\n' + body)
        (project / 'private.h').symlink_to(private_header)
        denied = audit('expanded-symlink-outside-authority', 'bypass', complete=False, reason='outside')
        assert all(item['resolvedPath'] != str(private_header) for item in denied['inputs']), denied
        audit('expanded-symlink-explicitly-authorized', 'miss', extra=['--analysis-input-root', str(external)])
        audit('authorized-symlink-warm', 'hit', extra=['--analysis-input-root', str(external)])
except Exception as error:
    evidence['failed'] += 1
    evidence['error'] = repr(error)
    raise
finally:
    destination = Path(args.evidence)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(evidence, indent=2) + '\n')

print(f'C++ audit macro header closure: {evidence["passed"]} scenarios passed; 0 failed; 0 skipped')
