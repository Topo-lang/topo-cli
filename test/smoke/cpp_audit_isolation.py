#!/usr/bin/env python3
"""Exercise C++ audit data permissions and startup-environment confinement."""
import argparse
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
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='topo-cpp-audit-isolation-') as temporary:
    root = Path(temporary).resolve()
    project, external, tools = root / 'project', root / 'external', root / 'tools'
    for directory in (project, external, tools):
        directory.mkdir()
    native = tools / 'topo-extract-cpp'
    shutil.copy2(Path(args.extractor_dir) / 'topo-extract-cpp', native)
    check = root / 'topo-check'
    shutil.copy2(args.check, check)
    env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ.get('PATH', ''))
    (project / 'main.topo').write_text('namespace app { public: void run(); }\n')
    (project / 'Topo.toml').write_text('[topo]\nroot="main.topo"\n[build]\nlanguage="cpp"\nsources=["*.cpp"]\n')
    (project / 'Topo.audit.toml').write_text('version=1\n[[manual]]\nkind="file"\nfile="main.cpp"\n')
    (external / 'value.h').write_text('#define VALUE 7\n')
    (external / 'ambient.h').write_text('#define VALUE 99\n')
    source = project / 'main.cpp'
    source.write_text('#include <value.h>\nnamespace app { void run() { int value = VALUE; (void)value; } }\n')
    model = project / 'compile_commands.json'

    def command(compiler=args.compiler, include=True):
        argv = [compiler, '--no-default-config', '-std=c++17', '-nostdinc', '-nostdinc++']
        if include:
            argv += ['-I', str(external)]
        argv += ['-c', 'main.cpp']
        model.write_text(json.dumps([{'directory': str(project), 'file': 'main.cpp', 'arguments': argv}]))

    command()
    report = root / 'report.json'
    base = [str(check), '--project', str(project), '--trust-store', str(root / 'trust/store.json')]
    runs = 0

    def audit(complete, extra=(), diagnostic=None, expected=1):
        global runs
        result = subprocess.run(base + ['--audit', '--audit-report', str(report), '--json'] + list(extra),
                                env=env, capture_output=True, text=True, timeout=90)
        assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
        data = json.loads(report.read_text())
        assert data['analysisComplete'] == complete, data
        if complete:
            assert data['analysisIsolation'] == 'macos-seatbelt', data
            assert data['declarationsPassed'] and data['manualApproval'] == ('accepted' if expected == 0 else 'pending'), data
        if diagnostic:
            assert diagnostic in json.dumps(data), (diagnostic, data)
        runs += 1
        return data

    audit(False, diagnostic='outside the project and explicit input roots')
    grant = ['--analysis-input-root', str(external)]
    accepted = audit(True, grant)
    extra = root / 'other-inputs'
    extra.mkdir()
    changed_scope = audit(True, grant + ['--analysis-input-root', str(extra)])
    assert changed_scope['reviewKey'] != accepted['reviewKey']

    # An inherited include path grants neither search semantics nor input access.
    command(include=False)
    source.write_text('#include <ambient.h>\nnamespace app { void run() {} }\n')
    env['CPATH'] = str(external)
    cleared = audit(False, grant, 'ambient.h')
    assert 'ambient compiler environment' not in json.dumps(cleared), cleared
    del env['CPATH']
    command()
    source.write_text('#include <value.h>\nnamespace app { void run() {} }\n')

    # If DYLD injection reaches the extractor, its initializer creates a real
    # sentinel. The initializer intentionally ignores the parent topo-check.
    sentinel = root / 'injected-into-extractor'
    injection = root / 'injection.c'
    injection.write_text('extern int _NSGetExecutablePath(char*, unsigned*);\n'
                         'extern char* strrchr(const char*, int);\n'
                         'extern int strcmp(const char*, const char*);\n'
                         'extern int open(const char*, int, ...);\nextern int close(int);\nextern void _exit(int);\n'
                         '__attribute__((constructor)) static void injected(void) {\n'
                         'char path[4096]; unsigned size=sizeof(path);\n'
                         'if (_NSGetExecutablePath(path,&size)) return;\n'
                         'char* name=strrchr(path,47); if (!name) return;\n'
                         'if (strcmp(name+1,"topo-extract-cpp")) return;\n'
                         f'int fd=open({json.dumps(str(sentinel))}, 0x0201, 0600); if(fd>=0)close(fd); _exit(86);\n}}\n')
    library = root / 'injection.dylib'
    built = subprocess.run([args.compiler, '-dynamiclib', '-x', 'c', str(injection), '-o', str(library)],
                           capture_output=True, text=True, timeout=60)
    assert built.returncode == 0, (built.stdout, built.stderr)
    env['DYLD_INSERT_LIBRARIES'] = str(library)
    audit(True, grant)
    assert not sentinel.exists(), 'startup injection reached the analysis extractor'
    del env['DYLD_INSERT_LIBRARIES']

    # A compiler path in the project model cannot select an executable or add
    # arbitrary file-read permissions, even when its directory is explicitly read.
    compiler_sentinel = root / 'compiler-was-run'
    fake_compiler = external / 'clang++'
    fake_compiler.write_text('#!/bin/sh\ntouch ' + str(compiler_sentinel) + '\n')
    fake_compiler.chmod(0o700)
    command(str(fake_compiler))
    audit(False, grant, 'independently discovered installed native Clang')
    assert not compiler_sentinel.exists()
    command()

    script_sentinel = root / 'script-extractor-was-run'
    native.unlink()
    native.write_text('#!/bin/sh\ntouch ' + str(script_sentinel) + '\n')
    native.chmod(0o700)
    audit(False, grant, 'installed native extractor')
    assert not script_sentinel.exists()

    # A replacement startup helper cannot consume an approval/cache produced by
    # the original execution chain. Only the local test copy is replaced.
    native.unlink()
    shutil.copy2(Path(args.extractor_dir) / 'topo-extract-cpp', native)
    launchers = [Path(item['path']) for item in accepted['inputs']
                 if Path(item['path']).name == 'topo-analysis-launcher']
    assert len(launchers) == 1, launchers
    helper = root / 'topo-analysis-launcher'
    shutil.copy2(launchers[0], helper)
    env['TOPO_AUDIT_CACHE_DIR'] = str(root / 'cache')
    warmed = audit(True, grant)
    accepted_review = subprocess.run(base + ['--accept-review', str(report)], env=env,
                                    capture_output=True, text=True, timeout=30)
    assert accepted_review.returncode == 0, accepted_review.stderr
    assert audit(True, grant, expected=0)['factsCache']['status'] == 'hit'
    failing_helper = root / 'failing-helper.c'
    failing_helper.write_text('int main(void) { return 86; }\n')
    built = subprocess.run([args.compiler, str(failing_helper), '-o', str(root / 'replacement-helper')],
                           capture_output=True, text=True, timeout=60)
    assert built.returncode == 0, (built.stdout, built.stderr)
    shutil.copy2(root / 'replacement-helper', helper)
    refused = audit(False, grant, 'runtime probe failed')
    assert not refused['passed'] and refused['factsCache']['status'] != 'hit'
    assert refused['reviewKey'] != warmed['reviewKey']
    print(f'C++ audit OS isolation: {runs} scenarios passed; 0 skipped')
