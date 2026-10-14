#!/usr/bin/env python3
"""Bind ordinary CI dependency caches and source checkouts to resolved commits."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

REPOSITORIES = {
    'topo-core': 'TOPO_CORE_REF', 'topo-lang': 'TOPO_LANG_REF',
    'topo-v8': 'TOPO_V8_REF', 'topo-lang-cpp': 'TOPO_LANG_CPP_REF',
    'topo-lang-rust': 'TOPO_LANG_RUST_REF', 'topo-lang-java': 'TOPO_LANG_JAVA_REF',
    'topo-lang-python': 'TOPO_LANG_PYTHON_REF',
    'topo-lang-typescript': 'TOPO_LANG_TYPESCRIPT_REF',
    'topo-llvm': 'TOPO_LLVM_REF', 'topo-jvm': 'TOPO_JVM_REF',
}


def git(*args):
    return subprocess.check_output(['git', *map(str, args)], text=True).strip()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def resolve(repository, ref):
    if re.fullmatch('[0-9a-fA-F]{40}', ref):
        return ref.lower()
    url = f'https://github.com/Topo-lang/{repository}.git'
    candidates = [f'refs/heads/{ref}', f'refs/tags/{ref}^{{}}', f'refs/tags/{ref}']
    if ref.startswith('refs/'):
        candidates = [ref + '^{}', ref]
    rows = git('ls-remote', '--exit-code', url, *candidates).splitlines()
    refs = {name: sha for sha, name in (row.split() for row in rows)}
    for candidate in candidates:
        if candidate in refs:
            return refs[candidate]
    raise RuntimeError(f'{repository}: cannot resolve {ref}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    resolution = sub.add_parser('resolve')
    resolution.add_argument('--scope', choices=['smoke', 'integrated'], required=True)
    checkout = sub.add_parser('checkout')
    checkout.add_argument('repository', choices=REPOSITORIES)
    checkout.add_argument('directory', type=Path)
    install = sub.add_parser('install-identity')
    install.add_argument('--prefix', type=Path, required=True)
    install.add_argument('--cache-hit', choices=['true', 'false', ''], required=True)
    args = parser.parse_args()
    evidence = Path(os.environ['RUNNER_TEMP'])
    manifest = evidence / 'topo-upstream-identities.json'
    actual_file = evidence / 'topo-upstream-checkouts.json'
    if args.command == 'resolve':
        rows = {}
        for repository, variable in REPOSITORIES.items():
            if args.scope == 'smoke' and repository in ('topo-llvm', 'topo-jvm'):
                continue
            ref = os.environ[variable]
            rows[repository] = {'ref': ref, 'sha': resolve(repository, ref)}
        save(manifest, rows)
        identities = {repository: row['sha'] for repository, row in rows.items()}
        key = hashlib.sha256(json.dumps(identities, sort_keys=True).encode()).hexdigest()
        with Path(os.environ['GITHUB_ENV']).open('a', encoding='utf-8') as stream:
            stream.write(f'TOPO_UPSTREAM_CACHE_KEY={key}\n')
        cli_sha = git('rev-parse', 'HEAD')
        if cli_sha != os.environ.get('GITHUB_SHA', cli_sha):
            raise RuntimeError('topo-cli checkout does not match the workflow commit')
        save(evidence / 'topo-cli-source-identity.json', {'repository': 'Topo-lang/topo-cli', 'sha': cli_sha})
        print(json.dumps({'cacheKey': key, 'upstreams': rows}, indent=2), flush=True)
        return
    expected = json.loads(manifest.read_text(encoding='utf-8'))
    if args.command == 'checkout':
        sha = expected[args.repository]['sha']
        directory = args.directory
        if not (directory / '.git').exists():
            if directory.exists() and any(directory.iterdir()):
                raise RuntimeError(f'{directory}: refusing to reuse a non-checkout directory')
            url = f'https://github.com/Topo-lang/{args.repository}.git'
            subprocess.run(['git', 'init', str(directory)], check=True)
            subprocess.run(['git', '-C', str(directory), 'remote', 'add', 'origin', url], check=True)
            subprocess.run(['git', '-C', str(directory), 'fetch', '--depth', '1', 'origin', sha], check=True)
            subprocess.run(['git', '-C', str(directory), 'checkout', '--detach', 'FETCH_HEAD'], check=True)
        actual = git('-C', directory, 'rev-parse', 'HEAD')
        if actual != sha:
            raise RuntimeError(f'{args.repository}: expected {sha}, checked out {actual}')
        rows = json.loads(actual_file.read_text(encoding='utf-8')) if actual_file.exists() else {}
        rows[args.repository] = actual
        save(actual_file, rows)
        print(f'Verified Topo-lang/{args.repository} at {actual}', flush=True)
        return
    marker = args.prefix / 'upstream-source-identities.json'
    if args.cache_hit == 'true':
        if not marker.exists():
            raise RuntimeError('restored install has no source provenance')
        recorded = json.loads(marker.read_text(encoding='utf-8'))
        if {repository: row['sha'] for repository, row in recorded.items()} != {repository: row['sha'] for repository, row in expected.items()}:
            raise RuntimeError('restored install provenance does not match resolved upstream commits')
    else:
        actual = json.loads(actual_file.read_text(encoding='utf-8'))
        if actual != {repository: row['sha'] for repository, row in expected.items()}:
            raise RuntimeError('the installed dependency source set was not completely verified')
        save(marker, expected)
    print('Verified installed upstream source identities:\n' + json.dumps(expected, indent=2), flush=True)


if __name__ == '__main__':
    main()
