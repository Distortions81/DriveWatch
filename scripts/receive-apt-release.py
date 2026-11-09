#!/usr/bin/env python3
"""Forced SSH command for publishing this project's release packages only."""
import hashlib
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import tempfile

MAX_PACKAGE_BYTES = 64 * 1024 * 1024
PACKAGE = 'm45-drive-watcher'


def parse_upload(command):
    words = shlex.split(command)
    if len(words) != 5 or words[0] != 'publish' or words[1] != PACKAGE:
        raise ValueError('Only m45-drive-watcher package publication is allowed.')
    _, name, version, size, checksum = words
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version):
        raise ValueError('Invalid release version.')
    if not size.isdigit() or not 0 < int(size) <= MAX_PACKAGE_BYTES:
        raise ValueError('Invalid package size.')
    if not re.fullmatch(r'[0-9a-f]{64}', checksum):
        raise ValueError('Invalid package checksum.')
    return name, version, int(size), checksum


def receive(command, stream, state, publish):
    name, version, expected_size, expected_hash = parse_upload(command)
    incoming = state / 'incoming'
    incoming.mkdir(mode=0o700, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=incoming) as directory:
        package = Path(directory) / 'release.deb'
        digest, size = hashlib.sha256(), 0
        with package.open('xb') as output:
            while True:
                chunk = stream.read(min(1024 * 1024, expected_size - size + 1))
                if not chunk:
                    break
                size += len(chunk)
                if size > expected_size:
                    raise ValueError('Upload exceeds declared package size.')
                digest.update(chunk)
                output.write(chunk)
        if size != expected_size or digest.hexdigest() != expected_hash:
            raise ValueError('Upload size or checksum does not match.')
        control = subprocess.check_output(['dpkg-deb', '-f', str(package), 'Package', 'Version', 'Architecture'], timeout=30).decode()
        fields = dict(line.split(': ', 1) for line in control.splitlines())
        if (fields.get('Package'), fields.get('Version'), fields.get('Architecture')) != (name, version, 'amd64'):
            raise ValueError('Package identity, version, or architecture does not match the release.')
        publish(package)


def main():
    # SSH callers cannot choose paths, commands, archive keys, or another package.
    state = Path('/home/dist/.local/share/m45-apt')
    repository = Path('/var/www/m45software.com/apt')
    command = os.environ.get('SSH_ORIGINAL_COMMAND', '')
    if command == 'check':
        if not repository.is_dir() or not list((state / 'gnupg/private-keys-v1.d').glob('*.key')):
            raise ValueError('Archive signing configuration is unavailable.')
        print('M45 APT receiver ready: m45-drive-watcher / amd64')
        return

    def publish(package):
        fingerprint = (state / 'signing-key.fingerprint').read_text().strip()
        subprocess.run([sys.executable, str(state / 'publish-apt.py'), '--repository', str(repository),
                        '--gnupghome', str(state / 'gnupg'), '--signing-key', fingerprint, str(package)],
                       check=True, timeout=180)

    signal.alarm(300)
    receive(command, sys.stdin.buffer, state, publish)
    print('Release package published and archive metadata signed.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print('APT publication refused: ' + str(error), file=sys.stderr)
        sys.exit(1)
