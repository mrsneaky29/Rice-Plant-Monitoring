"""Exercise real build/OTA script decisions with synthetic files and mocked I/O."""
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import urllib.request

import pytest


DEPLOY = Path(__file__).resolve().parents[1] / 'deploy'


def run_script(name, root, args, monkeypatch):
    monkeypatch.setattr(sys, 'argv', [name, *args])
    source = (DEPLOY / name).read_text(encoding='utf-8')
    exec(compile(source, str(DEPLOY / name), 'exec'),
         {'__name__': '__main__', '__file__': str(root / 'deploy' / name)})


def test_status_only_build_preserves_normal_image_and_resets_next_build_mode(tmp_path, monkeypatch):
    source = tmp_path / 'firmware' / 'ESP32_Station'
    source.mkdir(parents=True)
    (source / 'ESP32_Station.ino').write_text('// synthetic sketch')
    (source / 'secrets.h').write_text('// synthetic network configuration')
    private_station = tmp_path / '.private' / 'plant-2'
    private_station.mkdir(parents=True)
    (private_station / 'cloud_secrets.h').write_text('// synthetic device configuration')
    normal = tmp_path / '.private' / 'images' / 'plant-2'
    normal.mkdir(parents=True)
    original = normal / 'ESP32_Station.ino.bin'
    original.write_bytes(b'existing-normal-image')

    def compile_fake(command, check):
        assert check is True
        output = Path(command[command.index('--output-dir') + 1])
        (output / 'ESP32_Station.ino.bin').write_bytes(b'synthetic-compiled-image')

    monkeypatch.setattr(subprocess, 'run', compile_fake)
    run_script('build_firmware.py', tmp_path, ['plant-2', '--status-only', '--cli', 'synthetic-cli'], monkeypatch)
    mode_header = tmp_path / '.private' / 'build' / 'ESP32_Station' / 'cloud_run_mode.h'
    assert '#define RICE_CLOUD_STATUS_ONLY 1' in mode_header.read_text()
    assert original.read_bytes() == b'existing-normal-image'
    manifest = json.loads((normal / 'status-only' / 'manifest.json').read_text())
    assert manifest['mode'] == 'status-only'
    assert manifest['station_id'] == 'plant-2'
    assert manifest['sha256'] == hashlib.sha256(b'synthetic-compiled-image').hexdigest()

    run_script('build_firmware.py', tmp_path, ['plant-2', '--cli', 'synthetic-cli'], monkeypatch)
    assert '#define RICE_CLOUD_STATUS_ONLY 0' in mode_header.read_text()
    assert json.loads((normal / 'manifest.json').read_text())['mode'] == 'normal'


@pytest.mark.parametrize('status_only', [True, False])
def test_flash_rejects_wrong_mode_before_reading_credentials_or_calling_ota(tmp_path, monkeypatch, status_only):
    output = tmp_path / '.private' / 'images' / 'plant-2'
    if status_only:
        output /= 'status-only'
    output.mkdir(parents=True)
    (output / 'manifest.json').write_text(json.dumps({
        'station_id': 'plant-2', 'mode': 'normal' if status_only else 'status-only'}))
    calls = []

    def status_response(url, timeout):
        calls.append(url)
        return io.BytesIO(b'{"station_id":"plant-2"}')

    monkeypatch.setattr(urllib.request, 'urlopen', status_response)
    args = ['plant-2', '192.168.1.204', '--espota', 'does-not-exist.py']
    if status_only:
        args.append('--status-only')
    with pytest.raises(SystemExit, match='mode does not match'):
        run_script('flash_ota.py', tmp_path, args, monkeypatch)
    assert calls == ['http://192.168.1.204/status']
