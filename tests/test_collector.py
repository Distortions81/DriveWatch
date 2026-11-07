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
            self.assertEqual(argv, [c.SMARTCTL, '-x', '-j', '/dev/nvme0'])
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
