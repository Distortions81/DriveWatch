#!/usr/bin/env python3
"""Build a bootstrap package containing only the public archive key and APT source."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def build(key, output):
    public_data = key.read_bytes()
    if b'-----BEGIN PGP PUBLIC KEY BLOCK-----' not in public_data or b'PRIVATE KEY' in public_data:
        raise ValueError('Only an armored public key belongs in this package.')
    with tempfile.TemporaryDirectory() as directory:
        inspect = Path(directory) / 'inspect-key'
        inspect.mkdir(mode=0o700)
        info = subprocess.check_output(['gpg', '--homedir', str(inspect), '--batch', '--with-colons', '--show-keys', str(key.resolve())]).decode()
        epoch = int(next(line.split(':')[5] for line in info.splitlines() if line.startswith('pub:')))
        root = Path(directory) / 'm45-software-archive-keyring'
        control = root / 'DEBIAN'
        control.mkdir(parents=True)
        (control / 'control').write_text(
            'Package: m45-software-archive-keyring\nVersion: 1.0\nArchitecture: all\n'
            'Section: misc\nPriority: optional\nMaintainer: M45 Software\nDepends: ca-certificates\n'
            'Description: M45 Software archive signing key and APT source\n'
            ' Enables the signed M45 Software stable package repository.\n')
        (control / 'conffiles').write_text('/etc/apt/sources.list.d/m45-software.sources\n')
        public = root / 'usr/share/keyrings/m45-software-archive-keyring.asc'
        public.parent.mkdir(parents=True)
        public.write_bytes(public_data)
        source = root / 'etc/apt/sources.list.d/m45-software.sources'
        source.parent.mkdir(parents=True)
        source.write_text('Types: deb\nURIs: https://m45software.com/apt/\nSuites: stable\n'
                          'Components: main\nArchitectures: amd64\n'
                          'Signed-By: /usr/share/keyrings/m45-software-archive-keyring.asc\n')
        for path in [root, *root.rglob('*')]:
            path.chmod(0o755 if path.is_dir() else 0o644)
            os.utime(path, (epoch, epoch))
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['dpkg-deb', '--root-owner-group', '--build', str(root), str(output)], check=True, env={**os.environ, 'SOURCE_DATE_EPOCH': str(epoch)})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--public-key', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    arguments = parser.parse_args()
    build(arguments.public_key, arguments.output)
