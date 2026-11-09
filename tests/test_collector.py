import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('collect', Path(__file__).parents[1] / 'collector/collect.py')
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)


class CollectorTests(unittest.TestCase):
    def nvme(self):
        return {'model_name': 'Test SSD', 'serial_number': 'TEST-1', 'smart_status': {'passed': True},
                'temperature': {'current': 45}, 'nvme_smart_health_information_log': {
                    'critical_warning': 0, 'media_errors': 1135, 'available_spare': 92,
                    'available_spare_threshold': 10, 'percentage_used': 3,
                    'warning_temp_time': 132, 'critical_comp_time': 1}}

    def test_nvme_fields_and_identity(self):
        a = c.normalize(self.nvme(), '/dev/nvme0', '/sys/a')
        b = c.normalize(self.nvme(), '/dev/nvme3', '/sys/b')
        self.assertEqual(a['id'], b['id'])
        self.assertEqual(a['counters']['media_errors'], '1135')
        self.assertEqual(a['counters']['hot_minutes'], '1')
        self.assertEqual(a['available_spare'], 92)
        self.assertEqual(a['temperature'], 45)

    def test_unknown_is_not_zero(self):
        result = c.normalize({}, '/dev/sda', '/sys/a')
        self.assertNotIn('passed', result)
        self.assertNotIn('temperature', result)
        self.assertEqual(result['counters'], {})
        self.assertIn('error', result)

    def test_ata_attributes_and_statistics(self):
        result = c.normalize({'smart_status': {'passed': True}, 'ata_smart_attributes': {'table': [
            {'id': 5, 'raw': {'value': 2}}, {'id': 197, 'raw': {'value': 1}},
            {'id': 184, 'name': 'End_to_End_Error', 'when_failed': 'now', 'raw': {'value': 3}}]},
            'ata_device_statistics': {'pages': [{'table': [
                {'name': 'Number of Reported Uncorrectable Errors', 'value': 3},
                {'name': 'Number of Interface CRC Errors', 'value': 1},
                {'name': 'Percentage Used Endurance Indicator', 'value': 7}]}]}}, '/dev/sda', '/sys/a')
        self.assertEqual(result['counters']['uncorrectable'], '3')
        self.assertEqual(result['counters']['pending'], '1')
        self.assertEqual(result['percentage_used'], 7)
        self.assertEqual(result['failing_attributes'], ['End_to_End_Error'])

    def test_bx100_temperature_fallback(self):
        result = c.normalize({'temperature': {'current': 0}, 'ata_sct_temperature_history': {
            'table': [29, 30]}}, '/dev/sda', '/sys/a')
        self.assertEqual(result['temperature'], 30)
        self.assertIn('SCT', result['notes'][-1])

    def run_with(self, data, status):
        def runner(argv, **kwargs):
            self.assertEqual(argv, [c.SMARTCTL, '-x', '--json=o', '/dev/nvme0'])
            self.assertNotIn('shell', kwargs)
            self.assertEqual(kwargs['timeout'], 90)
            return subprocess.CompletedProcess(argv, status, json.dumps(data))
        return c.collect_one('/dev/nvme0', '/sys/a', runner)

    def test_historical_error_exit_bits_are_not_read_failures(self):
        result = self.run_with(self.nvme(), 64 | 128)
        self.assertNotIn('error', result)
        self.assertTrue(result['passed'])

    def test_failing_health_exit_bit(self):
        result = self.run_with(self.nvme(), 8)
        self.assertFalse(result['passed'])

    def test_partial_commands_keep_readable_health(self):
        result = self.run_with(self.nvme(), 4)
        self.assertNotIn('error', result)
        self.assertIn('optional', result['notes'][-1])

    def test_read_failure_stays_visible(self):
        result = self.run_with({'smartctl': {'exit_status': 2}}, 2)
        self.assertIn('error', result)
        self.assertEqual(result['device'], '/dev/nvme0')

    def test_real_smartctl_accepts_collector_json_option(self):
        if not Path(c.SMARTCTL).exists():
            self.skipTest('smartctl is not installed')
        # The device is intentionally nonexistent: exercise option parsing without drive IO.
        result = c.collect_one('/dev/m45-drive-watcher-test-nonexistent', '/sys/nonexistent')
        self.assertNotIn('UNRECOGNIZED OPTION', result['error'])
        self.assertIn('smartctl', result.get('diagnostics', ''))
        self.assertIn('m45-drive-watcher-test-nonexistent', result['error'])

    def test_identity_failure_keeps_actual_diagnostic(self):
        output = ['smartctl 7.4', 'Read Device Identity failed: scsi error unsupported field in scsi command']
        result = self.run_with({'smartctl': {'exit_status': 2, 'output': output}}, 2)
        self.assertIn('unsupported field', result['error'])
        self.assertIn('Read Device Identity failed', result['diagnostics'])
        self.assertEqual(result['smart_support'], 'unavailable')

    def test_timeout_and_malformed_output(self):
        def timeout(*args, **kwargs):
            self.assertEqual(kwargs['timeout'], 90)
            raise subprocess.TimeoutExpired('smartctl', 90)
        self.assertIn('error', c.collect_one('/dev/sda', '/sys/a', timeout))
        def malformed(*args, **kwargs):
            return subprocess.CompletedProcess([], 0, 'not json')
        self.assertIn('error', c.collect_one('/dev/sda', '/sys/a', malformed))

    def test_physical_discovery_excludes_partitions_and_virtual_devices(self):
        with tempfile.TemporaryDirectory() as path:
            root = Path(path)
            for name in ['sda', 'sda1', 'nvme0n1', 'nvme0n2', 'nvme0n1p1', 'loop0', 'dm-0', 'mmcblk0']:
                (root / name).mkdir()
            self.assertEqual([d[0] for d in c.discover(root)], ['/dev/mmcblk0', '/dev/nvme0n1', '/dev/sda'])

    def test_system_names_and_partition_labels_survive_smart_failure(self):
        payload = {'blockdevices': [{'path': '/dev/sdb', 'model': 'Expansion SW',
            'serial': 'USB123', 'tran': 'usb', 'size': 8000000000000,
            'children': [{'path': '/dev/sdb1', 'label': 'Backups',
                          'mountpoints': ['/media/Backups', None]}]}]}
        def lsblk(argv, **kwargs):
            return subprocess.CompletedProcess(argv, 0, json.dumps(payload))
        metadata = c.system_metadata(lsblk)['/dev/sdb']
        def failed(argv, **kwargs):
            return subprocess.CompletedProcess(argv, 2, '{"smartctl":{"exit_status":2}}')
        result = c.collect_one('/dev/sdb', '/sys/usb1/test', failed, metadata)
        self.assertEqual(result['model'], 'Expansion SW')
        self.assertEqual(result['serial'], 'USB123')
        self.assertTrue(result['external'])
        self.assertEqual(result['partitions'][0]['label'], 'Backups')
        self.assertEqual(result['partitions'][0]['mountpoints'], ['/media/Backups'])
        self.assertEqual(result['smart_support'], 'unavailable')
        def timed_out(*args, **kwargs):
            raise subprocess.TimeoutExpired('smartctl', 90)
        timeout = c.collect_one('/dev/sdb', '/sys/usb1/test', timed_out, metadata)
        self.assertEqual(timeout['id'], result['id'])
        self.assertEqual(timeout['model'], 'Expansion SW')
        moved = c.collect_one('/dev/sdd', '/sys/usb2/test', failed, metadata)
        self.assertEqual(moved['id'], result['id'])

    def test_unsupported_differs_from_unavailable(self):
        result = c.normalize({'smart_support': {'available': False}}, '/dev/sdb', '/sys/a')
        self.assertEqual(result['smart_support'], 'unsupported')
        self.assertNotIn('passed', result)
        self.assertEqual(c.normalize({}, '/dev/sdb', '/sys/a')['smart_support'], 'unavailable')

    def test_metadata_failure_does_not_stop_smart(self):
        def failed(*args, **kwargs):
            raise subprocess.TimeoutExpired('lsblk', 10)
        self.assertEqual(c.system_metadata(failed), {})
        self.assertTrue(c.normalize(self.nvme(), '/dev/nvme0', '/sys/a', {})['passed'])

    def test_usb_speed_uses_nearest_device_not_hub(self):
        with tempfile.TemporaryDirectory() as directory:
            hub = Path(directory) / 'usb2/hub'
            disk = hub / 'disk'
            leaf = disk / 'block/sdb'
            leaf.mkdir(parents=True)
            for path, speed in [(hub, '10000'), (disk, '5000')]:
                (path / 'idVendor').write_text('0bc2')
                (path / 'speed').write_text(speed)
            self.assertEqual(c.link_metadata(str(leaf), 'usb')['connection_speed'], '5 Gb/s')
            (disk / 'speed').unlink()
            self.assertNotIn('connection_speed', c.link_metadata(str(leaf), 'usb'))

    def test_nvme_link_uses_endpoint_not_upstream_port(self):
        with tempfile.TemporaryDirectory() as directory:
            upstream = Path(directory) / '0000:00:01.0'
            endpoint = upstream / '0000:04:00.0'
            leaf = endpoint / 'nvme/nvme0/nvme0n1'
            leaf.mkdir(parents=True)
            for path, speed, width in [(upstream, '16.0 GT/s PCIe', '8'), (endpoint, '8.0 GT/s PCIe', '4')]:
                (path / 'current_link_speed').write_text(speed)
                (path / 'current_link_width').write_text(width)
            self.assertEqual(c.link_metadata(str(leaf), 'nvme')['connection_speed'], 'PCIe 8.0 GT/s ×4')
            (endpoint / 'current_link_width').unlink()
            self.assertNotIn('connection_speed', c.link_metadata(str(leaf), 'nvme'))
            self.assertNotIn('connection_speed', c.link_metadata(str(leaf), 'sas'))

    def test_current_sata_speed_does_not_replace_usb_link_speed(self):
        data = {'interface_speed': {'max': {'string': '6.0 Gb/s'}, 'current': {'string': '3.0 Gb/s'}}}
        result = c.normalize(data, '/dev/sda', '/sys/a', {'transport': 'sata', 'connection_speed': '6.0 Gb/s'})
        self.assertEqual(result['connection_speed'], '3.0 Gb/s')
        result = c.normalize(data, '/dev/sdb', '/sys/a', {'transport': 'usb', 'connection_speed': '5 Gb/s'})
        self.assertEqual(result['connection_speed'], '5 Gb/s')

    def test_sata_speed_reads_its_port(self):
        with tempfile.TemporaryDirectory() as directory:
            ata = Path(directory) / 'ata1'
            leaf = ata / 'host0/block/sda'
            leaf.mkdir(parents=True)
            speed = ata / 'link1/ata_link/link1/sata_spd'
            speed.parent.mkdir(parents=True)
            speed.write_text('6.0 Gbps')
            self.assertEqual(c.link_metadata(str(leaf))['connection_speed'], '6.0 Gb/s')
            speed.write_text('<unknown>')
            self.assertNotIn('connection_speed', c.link_metadata(str(leaf)))

    def test_atomic_snapshot(self):
        with tempfile.TemporaryDirectory() as path:
            target = Path(path) / 'run/snapshot.json'
            c.write_snapshot({'version': 1, 'drives': []}, target)
            c.write_snapshot({'version': 1, 'drives': [1]}, target)
            self.assertEqual(json.loads(target.read_text())['drives'], [1])
            self.assertEqual(target.stat().st_mode & 0o777, 0o644)
            self.assertEqual(len(list(target.parent.iterdir())), 1)


if __name__ == '__main__':
    unittest.main()
