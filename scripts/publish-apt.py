#!/usr/bin/env python3
"""Publish M45 packages and signed, atomic APT metadata. Never exports private keys."""
import argparse
import email.utils
import fcntl
import gzip
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile


def run(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd)


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
            os.fchmod(stream.fileno(), 0o644)
        temporary.replace(path)
    finally:
        if temporary and temporary.exists():
            temporary.unlink()


def publish(repository, keyhome, fingerprint, packages):
    repository, keyhome = repository.resolve(), keyhome.resolve()
    if not re.fullmatch(r'[0-9A-Fa-f]{40}|[0-9A-Fa-f]{64}', fingerprint):
        raise ValueError('Use the full archive signing-key fingerprint.')
    if keyhome.is_relative_to(repository):
        raise ValueError('The private signing-key directory must be outside the public repository.')
    if keyhome.stat().st_mode & 0o077:
        raise ValueError('The signing-key directory must have mode 0700.')
    repository.mkdir(parents=True, exist_ok=True)
    gpg = ['gpg', '--homedir', str(keyhome), '--batch', '--yes', '--local-user', fingerprint]
    secret = run(*gpg, '--with-colons', '--list-secret-keys', fingerprint).decode()
    if fingerprint.upper() not in [line.split(':')[9] for line in secret.splitlines() if line.startswith('fpr:')]:
        raise ValueError('Archive signing key is not available.')
    recorded_key = repository / 'archive-key.fingerprint'
    if recorded_key.exists() and recorded_key.read_text().strip() != fingerprint.upper():
        raise ValueError('Refusing to replace the archive key; perform a deliberate key rotation.')
    with (keyhome.parent / 'publish.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        for package in packages:
            package = package.resolve(strict=True)
            fields = run('dpkg-deb', '-f', str(package), 'Package', 'Version', 'Architecture').decode()
            fields = dict(line.split(': ', 1) for line in fields.splitlines())
            name, version, architecture = (fields[k] for k in ('Package', 'Version', 'Architecture'))
            if not re.fullmatch(r'm45-[a-z0-9+.-]+', name):
                raise ValueError('Only m45-* packages belong in this archive.')
            if architecture not in ('amd64', 'all') or not re.fullmatch(r'[0-9A-Za-z.+:~_-]+', version):
                raise ValueError('Unsupported architecture or invalid version.')
            target = repository / 'pool/main' / f'{name}_{version}_{architecture}.deb'
            data = package.read_bytes()
            if target.exists() and target.read_bytes() != data:
                raise ValueError(f'An existing package version cannot change: {target.name}')
            if not target.exists():
                atomic_write(target, data)
        (repository / 'pool/main').mkdir(parents=True, exist_ok=True)
        indexes = run('dpkg-scanpackages', '--multiversion', 'pool', '/dev/null', cwd=repository)
        if not indexes.strip():
            raise ValueError('Refusing to publish an empty archive.')
        relative = Path('main/binary-amd64')
        files = {relative / 'Packages': indexes, relative / 'Packages.gz': gzip.compress(indexes, mtime=0)}
        release = '\n'.join([
            'Origin: M45 Software', 'Label: M45 Software', 'Suite: stable', 'Codename: stable',
            'Architectures: amd64', 'Components: main', 'Acquire-By-Hash: yes',
            'Description: M45 Software packages built on Ubuntu 24.04 (amd64)',
            'Date: ' + email.utils.formatdate(usegmt=True), 'SHA256:',
            *[f' {hashlib.sha256(data).hexdigest()} {len(data):16d} {path.as_posix()}' for path, data in files.items()],
            '',
        ]).encode()
        with tempfile.TemporaryDirectory(dir=keyhome.parent) as directory:
            stage = Path(directory)
            (stage / 'Release').write_bytes(release)
            subprocess.run([*gpg, '--digest-algo', 'SHA256', '--clearsign', '--output', str(stage / 'InRelease'), str(stage / 'Release')], check=True)
            subprocess.run([*gpg, '--digest-algo', 'SHA256', '--armor', '--detach-sign', '--output', str(stage / 'Release.gpg'), str(stage / 'Release')], check=True)
            # Keep old hash-addressed indexes so concurrent clients can finish an update.
            for path, data in files.items():
                digest = hashlib.sha256(data).hexdigest()
                atomic_write(repository / 'dists/stable' / path.parent / 'by-hash/SHA256' / digest, data)
                atomic_write(repository / 'dists/stable' / path, data)
            public_key = run('gpg', '--homedir', str(keyhome), '--batch', '--export-options', 'export-minimal', '--armor', '--export', fingerprint)
            atomic_write(repository / 'm45-software.asc', public_key)
            atomic_write(recorded_key, (fingerprint.upper() + '\n').encode())
            for name in ('Release.gpg', 'Release', 'InRelease'):
                atomic_write(repository / 'dists/stable' / name, (stage / name).read_bytes())
        print('Published signed stable/amd64 archive:', repository)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--gnupghome', type=Path, required=True)
    parser.add_argument('--signing-key', required=True)
    parser.add_argument('packages', nargs='*', type=Path)
    options = parser.parse_args()
    publish(options.repository, options.gnupghome, options.signing_key, options.packages)
