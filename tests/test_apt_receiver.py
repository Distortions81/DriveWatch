import hashlib
import importlib.util
import io
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('apt_receiver', ROOT / 'scripts/receive-apt-release.py')
receiver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(receiver)


class ReceiverTests(unittest.TestCase):
    def test_rejects_shell_commands_and_invalid_upload_parameters(self):
        for command in ['sh', 'uname -a', 'publish m45-other 1.0.0 1 ' + 'a' * 64,
                        'publish m45-drive-watcher ../../file 1 ' + 'a' * 64,
                        'publish m45-drive-watcher 1.0.0 0 ' + 'a' * 64,
                        'publish m45-drive-watcher 1.0.0 999999999 ' + 'a' * 64,
                        'publish m45-drive-watcher 1.0.0 1 $(id)']:
            with self.subTest(command=command), self.assertRaises(ValueError):
                receiver.parse_upload(command)

    @unittest.skipUnless(shutil.which('dpkg-deb'), 'dpkg-deb is required')
    def test_upload_checks_integrity_and_package_identity_before_publication(self):
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory)
            fixture = state / 'fixture/DEBIAN'
            fixture.mkdir(parents=True)
            (fixture / 'control').write_text('Package: m45-drive-watcher\nVersion: 1.2.3\nArchitecture: amd64\nMaintainer: M45 Test\nDescription: receiver fixture\n')
            package = state / 'fixture.deb'
            subprocess.run(['dpkg-deb', '--root-owner-group', '--build', str(fixture.parent), str(package)], check=True, capture_output=True)
            data = package.read_bytes()
            command = f'publish m45-drive-watcher 1.2.3 {len(data)} {hashlib.sha256(data).hexdigest()}'
            published = []
            receiver.receive(command, io.BytesIO(data), state, lambda p: published.append(p.read_bytes()))
            self.assertEqual(published, [data])
            for content, request in [(data + b'X', command), (data[:-1], command),
                                     (data, command.replace('1.2.3', '1.2.4')),
                                     (data, command[:-64] + '0' * 64)]:
                with self.assertRaises(ValueError):
                    receiver.receive(request, io.BytesIO(content), state, lambda p: self.fail('Rejected upload was published'))
            self.assertEqual(list((state / 'incoming').iterdir()), [])
