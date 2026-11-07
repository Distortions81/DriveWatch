#!/usr/bin/python3
"""Fixed, read-only SMART collector. Installed root-owned; accepts no CLI input."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

OUTPUT = Path('/run/drivewatch/snapshot.json')
SMARTCTL = '/usr/sbin/smartctl'
SMART_TIMEOUT_SECONDS = 90
DEVICE = re.compile(r'(sd[a-z]+|nvme\d+n\d+|mmcblk\d+|hd[a-z]+)\Z')


def number(value):
    # Keep absent and malformed fields unknown instead of manufacturing zeroes.
    return value if isinstance(value, (int, float)) and not isinstance(value, bool) and value >= 0 else None


def normalize(data, device, physical_path):
    model = data.get('model_name') or data.get('model_number') or data.get('product') or 'Unknown drive'
    serial = data.get('serial_number', '')
    identity = f'{model}|{serial}' if serial else f'{model}|{physical_path}'
    result = dict(id=hashlib.sha256(identity.encode()).hexdigest()[:32], device=device,
                  model=model, serial=serial, counters={}, notes=[])
    if not serial:
        result['notes'].append('No serial reported; identity is tied to the physical device path.')
    passed = data.get('smart_status', {}).get('passed')
    if isinstance(passed, bool):
        result['passed'] = passed
    temp = number(data.get('temperature', {}).get('current'))
    if temp is not None and 0 < temp < 150:
        result['temperature'] = temp
    result['power_on_hours'] = number(data.get('power_on_time', {}).get('hours'))
    counters = result['counters']
    nvme = data.get('nvme_smart_health_information_log')
    if isinstance(nvme, dict):
        result['protocol'] = 'NVMe'
        for source, target in [('critical_warning', 'critical_warning'), ('available_spare', 'available_spare'),
                               ('available_spare_threshold', 'spare_threshold'), ('percentage_used', 'percentage_used'),
                               ('power_on_hours', 'power_on_hours')]:
            value = number(nvme.get(source))
            if value is not None:
                result[target] = value
        for source, target in [('media_errors', 'media_errors'), ('unsafe_shutdowns', 'unsafe_shutdowns'),
                               ('num_err_log_entries', 'command_errors'), ('warning_temp_time', 'warm_minutes'),
                               ('critical_comp_time', 'hot_minutes')]:
            value = number(nvme.get(source))
            if value is not None:
                # Counters are decimal strings to retain precision in Qt JSON.
                counters[target] = str(int(value))
    else:
        result['protocol'] = data.get('device', {}).get('protocol', 'ATA')
        names = {5: 'reallocated', 184: 'end_to_end', 187: 'uncorrectable',
                 197: 'pending', 198: 'offline_uncorrectable', 199: 'crc_errors'}
        failing = []
        for attr in data.get('ata_smart_attributes', {}).get('table', []):
            key = names.get(attr.get('id'))
            value = number(attr.get('raw', {}).get('value'))
            if key and value is not None:
                counters[key] = str(int(value))
            if str(attr.get('when_failed', '')).lower() == 'now':
                failing.append(attr.get('name', str(attr.get('id'))))
        if failing:
            result['failing_attributes'] = failing
        stat_names = {'Number of Reported Uncorrectable Errors': 'uncorrectable',
                      'Number of Interface CRC Errors': 'crc_errors'}
        for page in data.get('ata_device_statistics', {}).get('pages', []):
            for stat in page.get('table', []):
                value = number(stat.get('value'))
                if value is None:
                    continue
                name = stat.get('name', '')
                if name in stat_names:
                    counters[stat_names[name]] = str(int(value))
                elif name == 'Percentage Used Endurance Indicator':
                    result['percentage_used'] = value
        # The BX100 sometimes reports current=0 while its temperature log is valid.
        if 'temperature' not in result:
            history = data.get('ata_sct_temperature_history', {}).get('table', [])
            for entry in reversed(history):
                value = number(entry)
                if value is not None and 0 < value < 150:
                    result['temperature'] = value
                    result['notes'].append('Temperature is the latest SCT history sample.')
                    break
    if 'passed' not in result and not isinstance(nvme, dict):
        result['error'] = 'Drive did not return SMART health status; monitoring coverage is incomplete.'
    if isinstance(nvme, dict) and number(nvme.get('critical_warning')) is None:
        result['error'] = 'Drive did not return NVMe critical-warning status; monitoring coverage is incomplete.'
    return result


def discover(sys_block=Path('/sys/class/block')):
    result = []
    controllers = set()
    for path in sorted(sys_block.iterdir()):
        if not DEVICE.fullmatch(path.name) or (path / 'partition').exists():
            continue
        nvme = re.fullmatch(r'(nvme\d+)n\d+', path.name)
        if nvme:
            if nvme[1] in controllers:
                continue
            controllers.add(nvme[1])
        result.append((f'/dev/{path.name}', str(path.resolve())))
    return result


def collect_one(device, physical_path, runner=subprocess.run):
    try:
        proc = runner([SMARTCTL, '-x', '-j', device], capture_output=True, text=True, timeout=SMART_TIMEOUT_SECONDS,
                      env={'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'LC_ALL': 'C'})
        data = json.loads(proc.stdout)
        if not isinstance(data, dict):
            raise ValueError('SMART output is not an object')
        result = normalize(data, device, physical_path)
        status = data.get('smartctl', {}).get('exit_status', proc.returncode)
        # smartctl uses a bit mask; nonzero can mean historical errors, not read failure.
        if status & 3:
            messages = '; '.join(m.get('string', '') for m in data.get('smartctl', {}).get('messages', []))
            result['error'] = messages or 'SMART device could not be opened.'
        elif status & 4:
            result['notes'].append('Some optional SMART commands were unsupported or failed.')
        if status & 8:
            result['passed'] = False
        return result
    except (OSError, subprocess.TimeoutExpired, ValueError, TypeError, AttributeError) as exc:
        # Keep an unreadable device visible in the UI.
        return dict(id=hashlib.sha256(physical_path.encode()).hexdigest()[:32], device=device,
                    model=Path(device).name, counters={}, error=f'SMART read failed: {exc}')


def write_snapshot(snapshot, output=OUTPUT):
    output.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
    temp_name = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', dir=output.parent, delete=False) as stream:
            temp_name = stream.name
            json.dump(snapshot, stream)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
            os.fchmod(stream.fileno(), 0o644)
        os.replace(temp_name, output)
    finally:
        if temp_name and os.path.exists(temp_name):
            os.unlink(temp_name)


def main():
    devices = discover()
    # Bound parallelism so a slow/unresponsive drive doesn't block all the others.
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        drives = list(pool.map(lambda d: collect_one(*d), devices))
    write_snapshot(dict(version=1, collected_at=int(time.time()), drives=drives))


if __name__ == '__main__':
    main()
