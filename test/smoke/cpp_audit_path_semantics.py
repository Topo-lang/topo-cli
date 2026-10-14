#!/usr/bin/env python3
"""Bind isolated audit facts and cache entries to physical symlink traversal."""
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
parser.add_argument('--compiler', required=True)
parser.add_argument('--evidence', required=True)
args = parser.parse_args()
evidence = {'scenarios': [], 'passed': 0, 'failed': 0, 'skipped': 0}

try:
    with tempfile.TemporaryDirectory(prefix='topo-cpp-path-semantics-') as temporary:
        root = Path(temporary).resolve()
        project = root / 'subject'
        project.mkdir()
        external = root / 'external'
        (external / 'nested').mkdir(parents=True)
        (project / 'left').mkdir()
        (project / 'right/nested').mkdir(parents=True)
        link = project / 'left/link'
        link.symlink_to('../right/nested')
        env = dict(os.environ, PATH=args.extractor_dir + os.pathsep + os.environ.get('PATH', ''))
        env['TOPO_AUDIT_CACHE_DIR'] = str(root / 'cache')
        evidence['environment'] = {'PATH': env['PATH'], 'TOPO_AUDIT_CACHE_DIR': env['TOPO_AUDIT_CACHE_DIR']}

        def write(name, text):
            path = project / name
            path.write_text(text)
            return path

        def digest(path):
            return hashlib.sha256(path.read_bytes()).hexdigest()

        body = 'namespace app { void run() { int observed = VALUE; (void)observed; } }\n'
        source = write('main.cpp', '#include <value.h>\n' + body)
        left = write('left/value.h', '#define VALUE 11\n')
        right = write('right/value.h', '#define VALUE 22\n')
        write('main.topo', 'namespace app { public: void run(); }\n')
        write('Topo.toml', '[topo]\nroot="main.topo"\n[build]\nlanguage="cpp"\nsources=["*.cpp"]\n')
        write('Topo.audit.toml', 'version=1\n[[manual]]\nkind="file"\nfile="main.cpp"\n')
        flags = ['--no-default-config', '-std=c++17', '-nostdinc', '-nostdinc++', '-Ileft/link/..']
        selected_model = project / 'compile_commands.json'

        def model(path=None, selected_source='main.cpp', extra_flags=None):
            target = path or selected_model
            target.write_text(json.dumps([{'directory': str(project), 'file': selected_source,
                'arguments': [args.compiler] + (flags if extra_flags is None else extra_flags) + ['-c', selected_source]}]))
            write('Topo.audit.toml', 'version=1\n[[manual]]\nkind="file"\nfile=' + json.dumps(selected_source) + '\n')

        model()
        report = root / 'report.json'
        base = [args.check, '--project', str(project), '--trust-store', str(root / 'reviewer/trust.json'),
                '--audit', '--audit-report', str(report), '--json']

        def audit(name, status, selected=(), rejected=(), complete=True, extra=()):
            report.unlink(missing_ok=True)
            command = base + ['--cpp-project', str(selected_model)] + list(extra)
            run = subprocess.run(command, cwd=root, env=env, capture_output=True, text=True, timeout=90)
            data = json.loads(report.read_text()) if report.exists() else None
            record = {'name': name, 'command': command, 'exitCode': run.returncode,
                      'stdout': run.stdout, 'stderr': run.stderr, 'report': data,
                      'linkTarget': os.readlink(link),
                      'files': {str(p.relative_to(root)): {'sha256': digest(p), 'text': p.read_text()}
                                for p in [source, left, right, selected_model]}}
            evidence['scenarios'].append(record)
            assert run.returncode == 1, record  # Manual approval remains pending.
            assert data['analysisComplete'] == complete, record
            assert data['factsCache']['status'] == status, record
            if complete:
                assert data['declarationsPassed'], record
                assert data['manualApproval'] == 'pending', record
            inputs = data['inputs']
            for path in selected:
                assert any(item['resolvedPath'] == str(path.resolve()) and item['sha256'] == digest(path)
                           for item in inputs), (str(path), record)
            for path in rejected:
                assert all(item['resolvedPath'] != str(path.resolve()) for item in inputs), (str(path), record)
            evidence['passed'] += 1
            return data

        command = [args.compiler] + flags + ['-E', '-P', 'main.cpp']
        clang = subprocess.run(command, cwd=project, capture_output=True, text=True, timeout=30)
        evidence['clang'] = {'command': command, 'cwd': str(project), 'exitCode': clang.returncode,
                             'stdout': clang.stdout, 'stderr': clang.stderr}
        assert clang.returncode == 0 and 'int observed = 22;' in clang.stdout, evidence['clang']

        cold = audit('search-path-cold', 'miss', [right], [left])
        assert audit('search-path-warm', 'hit', [right], [left])['reviewKey'] == cold['reviewKey']
        previous = right.stat()
        right.write_text('#define VALUE 23\n')
        os.utime(right, ns=(previous.st_atime_ns, previous.st_mtime_ns))
        changed = audit('physical-header-bytes-same-mtime', 'miss', [right], [left])
        assert changed['reviewKey'] != cold['reviewKey']
        audit('changed-header-warm', 'hit', [right], [left])
        left.write_text('#define VALUE 12\n')
        assert audit('unselected-lexical-header-change', 'hit', [right], [left])['reviewKey'] == changed['reviewKey']
        (project / 'left/nested').mkdir()
        link.unlink()
        link.symlink_to('nested')
        moved = audit('search-symlink-retargeted', 'miss', [left], [right])
        assert moved['reviewKey'] != changed['reviewKey']
        audit('retargeted-search-warm', 'hit', [left], [right])

        private_header = external / 'value.h'
        private_header.write_text('#define VALUE 44\n')
        link.unlink()
        link.symlink_to(external / 'nested')
        audit('physical-search-root-denied', 'bypass', rejected=[private_header], complete=False)
        granted = ['--analysis-input-root', str(external)]
        audit('physical-search-root-authorized', 'miss', [private_header], [left, right], extra=granted)
        audit('authorized-search-warm', 'hit', [private_header], [left, right], extra=granted)

        link.unlink()
        link.symlink_to('../right/nested')
        # The provider must open the selected model before converting its identity
        # to a canonical spelling. The lexically collapsed alternative is invalid.
        write('left/commands.json', 'invalid lexical alternative')
        selected_model = project / 'left/link/../commands.json'
        model()
        audit('selected-model-physical-path', 'miss', [right, selected_model], [left])
        audit('selected-model-warm', 'hit', [right, selected_model], [left])

        write('left/unit.cpp', 'invalid lexical source')
        physical_source = write('right/unit.cpp', '#include <value.h>\n' + body)
        model(selected_source='left/link/../unit.cpp')
        audit('selected-source-physical-path', 'miss', [physical_source, right], [project / 'left/unit.cpp'])
        audit('selected-source-warm', 'hit', [physical_source, right])

        write('left/flags.rsp', '-unknown-option')
        response = write('right/flags.rsp', ' '.join(flags[1:]))
        model(extra_flags=['--no-default-config', '@left/link/../flags.rsp'])
        audit('response-physical-path', 'miss', [response, right], [project / 'left/flags.rsp'])
        audit('response-warm', 'hit', [response, right])

        # Two spellings with the same lexical normalization still denote distinct
        # files. This literal '..' include is outside the current cache subset.
        first = write('left/which.h', '#define FIRST 11\n')
        second = write('right/which.h', '#define SECOND 22\n')
        source.write_text('#include "left/which.h"\n#include "left/link/../which.h"\n#define VALUE (FIRST + SECOND)\n' + body)
        model()
        audit('distinct-parsed-input-digests', 'bypass', [first, second])
except Exception as error:
    evidence['failed'] += 1
    evidence['error'] = repr(error)
    raise
finally:
    destination = Path(args.evidence)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(evidence, indent=2) + '\n')

print(f'C++ audit path semantics: {evidence["passed"]} passed; 0 failed; 0 skipped')
