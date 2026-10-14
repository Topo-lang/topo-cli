#!/usr/bin/env python3
"""Keep denied C++ audit model/cache paths out of parent discovery and reports."""
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
parser.add_argument('--observe', action='store_true', help='record vulnerable baseline without asserting fixes')
parser.add_argument('--evidence', required=True)
args = parser.parse_args()
results = []
with tempfile.TemporaryDirectory(prefix='topo-audit-parent-inputs-') as temporary:
    root = Path(temporary).resolve()
    project, external = root / 'project', root / 'external'
    project.mkdir()
    external.mkdir()
    env = dict(os.environ, PATH=args.extractor_dir + os.pathsep + os.environ.get('PATH', ''))
    (project / 'main.topo').write_text('namespace app { public: void run(); }\n')
    (project / 'Topo.toml').write_text('[topo]\nroot="main.topo"\n[build]\nlanguage="cpp"\nsources=["*.cpp"]\n')
    (project / 'Topo.audit.toml').write_text('version=1\n[[manual]]\nkind="file"\nfile="main.cpp"\n')
    source = project / 'main.cpp'
    source.write_text('namespace app { void run() {} }\n')
    outside = external / 'private.cpp'
    outside.write_text('namespace app { void run() {} } // synthetic private input\n')
    outside_digest = hashlib.sha256(outside.read_bytes()).hexdigest()
    report = root / 'report.json'

    def model(path, selected=source, compiler=args.compiler):
        path.write_text(json.dumps([{'directory': str(project), 'file': str(selected),
            'arguments': [compiler, '--no-default-config', '-std=c++17', '-nostdinc', '-nostdinc++', '-c', str(selected)]}]))
        return path

    def audit(selected=None, granted=()):
        argv = [args.check, '--project', str(project), '--audit', '--audit-report', str(report), '--json']
        if selected is not None:
            argv += ['--cpp-project', str(selected)]
        for grant in granted:
            argv += ['--analysis-input-root', str(grant)]
        run = subprocess.run(argv, env=env, cwd=root, capture_output=True, text=True, timeout=90)
        assert run.returncode == 1, (run.returncode, run.stdout, run.stderr)
        return json.loads(report.read_text())

    def record(name, data, expected, forbidden=()):
        inputs = {item['path']: item for item in data['inputs']}
        exposed = {item['path']: item for item in data['inputs']
                   if any(str(path) in (item['path'], item['resolvedPath']) for path in forbidden)}
        selected = [str(path) for path in forbidden if str(path) in data['sourceFiles']]
        results.append({'case': name, 'passed': bool(expected and not exposed and not selected),
                        'analysisComplete': data['analysisComplete'], 'factsCache': data['factsCache'],
                        'externalInputs': exposed, 'externalSources': selected,
                        'unresolved': data['unresolved']})

    external_model = model(external / 'compile_commands.json')
    denied = audit(external_model)
    record('external-model-denied', denied, not denied['analysisComplete'], [external_model, outside])
    granted = audit(external_model, [external])
    record('external-model-explicit-root', granted,
           granted['analysisComplete'] and str(external_model) in [item['path'] for item in granted['inputs']])

    local_model = model(project / 'compile_commands.json', outside)
    denied = audit(local_model)
    record('local-model-external-source-denied', denied, not denied['analysisComplete'], [outside])
    allowed_source = audit(local_model, [external])
    record('external-source-read-explicit-root', allowed_source,
           any(item['path'] == str(outside) and item['sha256'] == outside_digest for item in allowed_source['inputs'])
           and str(outside) in allowed_source['sourceFiles']
           and 'outside the project and explicit input roots' not in json.dumps(allowed_source['unresolved']))

    model(local_model, outside, 'g++')
    invalid = audit(local_model)
    record('invalid-model-row-does-not-select-source', invalid, not invalid['analysisComplete'], [outside])
    local_model.unlink()
    unconfigured = audit()
    record('missing-model-does-not-fall-back', unconfigured,
           not unconfigured['analysisComplete'] and not unconfigured['sourceFiles'], [source, outside])

    model(local_model)
    env['TOPO_AUDIT_CACHE_DIR'] = str(root / 'cache')
    cold = audit(local_model)
    record('valid-cache-cold', cold, cold['analysisComplete'] and cold['factsCache']['status'] == 'miss')
    warm = audit(local_model)
    record('valid-cache-warm', warm, warm['analysisComplete'] and warm['factsCache']['status'] == 'hit')
    entries = list((root / 'cache').rglob('*.json'))
    assert len(entries) == 1, entries
    valid = json.loads(entries[0].read_text())

    def inject(name, mutate, forbidden=()):
        entry = json.loads(json.dumps(valid))
        mutate(entry['payload'])
        canonical = json.dumps(entry['payload'], sort_keys=True, separators=(',', ':'), ensure_ascii=False)
        entry['payloadDigest'] = hashlib.sha256(canonical.encode()).hexdigest()
        entries[0].write_text(json.dumps(entry))
        data = audit(local_model)
        record(name, data, data['analysisComplete'] and data['factsCache']['status'] == 'miss', forbidden)

    inject('cache-selected-model-injection', lambda value: value.update(modelPath=str(external_model)), [external_model])
    inject('cache-selected-project-injection', lambda value: value.update(project=str(external)))

    def inject_digest(value):
        value['payload']['inputs'].append(str(outside))
        value['payload']['inputDigests'][str(outside)] = outside_digest
    inject('cache-digest-input-injection', inject_digest, [outside])
    inject('cache-identity-file-injection',
           lambda value: value['payload']['identities'][0].update(file=str(outside)), [outside])

    def inject_dependency(value):
        value['payload']['dependencies'].append({'id': 'synthetic-private-library', 'path': str(outside),
            'version': '', 'digest': outside_digest, 'project': False})
    inject('cache-dependency-path-injection', inject_dependency, [outside])

    # Parent CWD and projectRoot must never give one cached spelling two meanings.
    # These nested symlinks would turn project-relative report reads into access
    # to external data after a CWD-relative authorization check.
    nested = project / 'project'
    nested.mkdir()
    (nested / 'main.cpp').symlink_to(outside)
    (nested / 'compile_commands.json').symlink_to(external_model)
    inject('cache-relative-input-redirection',
           lambda value: value['payload']['inputs'].append('project/main.cpp'), [outside])
    inject('cache-relative-selected-model',
           lambda value: value.update(modelPath='project/compile_commands.json'), [external_model])
    inject('cache-relative-selected-project', lambda value: value.update(project='project'))
    inject('cache-empty-input-path', lambda value: value['payload']['inputs'].append(''))

Path(args.evidence).write_text(json.dumps(results, indent=2) + '\n')
passed = sum(item['passed'] for item in results)
print(f'C++ audit parent input confinement: {passed}/{len(results)} scenarios passed; 0 skipped')
if not args.observe:
    assert passed == len(results), [item['case'] for item in results if not item['passed']]
