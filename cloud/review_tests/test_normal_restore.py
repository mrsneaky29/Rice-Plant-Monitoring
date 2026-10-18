"""Normal-mode restoration guards, using synthetic images/configuration only."""
import io
import json
from pathlib import Path
import subprocess
import urllib.request

import pytest

from test_status_only_build import run_script


@pytest.mark.parametrize('opt_in', [False, True])
def test_build_requires_explicit_historical_upload_opt_in(tmp_path, monkeypatch, opt_in):
    source = tmp_path / 'firmware' / 'ESP32_Station'
    source.mkdir(parents=True)
    (source / 'ESP32_Station.ino').write_text('// synthetic sketch')
    (source / 'secrets.h').write_text('// synthetic credentials')
    station = tmp_path / '.private' / 'plant-2'
    station.mkdir(parents=True)
    (station / 'cloud_secrets.h').write_text('// synthetic device configuration')

    def fake_compile(command, check):
        out = Path(command[command.index('--output-dir') + 1])
        (out / 'ESP32_Station.ino.bin').write_bytes(b'synthetic firmware')

    monkeypatch.setattr(subprocess, 'run', fake_compile)
    args = ['plant-2', '--cli', 'synthetic-cli']
    if opt_in:
        args.append('--upload-existing-photos')
    run_script('build_firmware.py', tmp_path, args, monkeypatch)
    manifest = json.loads((tmp_path / '.private/images/plant-2/manifest.json').read_text())
    assert manifest['mode'] == 'normal'
    assert manifest['upload_existing_photos'] is opt_in
    assert manifest['sd_photo_protection'] is True
    header = (tmp_path / '.private/build/ESP32_Station/cloud_run_mode.h').read_text()
    assert f'#define RICE_CLOUD_UPLOAD_EXISTING_PHOTOS {int(opt_in)}' in header


def test_diagnostics_refuse_historical_upload_opt_in(tmp_path, monkeypatch):
    with pytest.raises(SystemExit):
        run_script('build_firmware.py', tmp_path,
                   ['plant-2', '--cli', 'unused', '--status-only', '--upload-existing-photos'], monkeypatch)
    assert not (tmp_path / '.private').exists()


def test_normal_flash_rejects_wrong_mac_before_private_files(tmp_path, monkeypatch):
    monkeypatch.setattr(urllib.request, 'urlopen', lambda *a, **kw:
                        io.BytesIO(b'{"station_id":"plant-2","mac":"AA:BB:CC:DD:EE:FF"}'))
    with pytest.raises(SystemExit, match='Hardware MAC mismatch'):
        run_script('flash_ota.py', tmp_path,
                   ['plant-2', '192.168.1.205', '--expected-mac', '14:2B:2F:C7:3A:2C',
                    '--espota', 'unused.py'], monkeypatch)


@pytest.mark.parametrize('policy', [None, True])
def test_normal_flash_rejects_unknown_or_unrequested_historical_policy(tmp_path, monkeypatch, policy):
    out = tmp_path / '.private/images/plant-2'
    out.mkdir(parents=True)
    manifest = {'station_id': 'plant-2', 'mode': 'normal'}
    if policy is not None:
        manifest['upload_existing_photos'] = policy
    (out / 'manifest.json').write_text(json.dumps(manifest))
    monkeypatch.setattr(urllib.request, 'urlopen', lambda *a, **kw:
                        io.BytesIO(b'{"station_id":"plant-2"}'))
    with pytest.raises(SystemExit, match='Existing-photo upload policy'):
        run_script('flash_ota.py', tmp_path,
                   ['plant-2', '192.168.1.205', '--espota', 'unused.py'], monkeypatch)


@pytest.mark.parametrize('protection', [None, False])
def test_normal_flash_requires_photo_protection_before_private_files(tmp_path, monkeypatch, protection):
    out = tmp_path / '.private/images/plant-2'
    out.mkdir(parents=True)
    manifest = {'station_id': 'plant-2', 'mode': 'normal', 'upload_existing_photos': False}
    if protection is not None:
        manifest['sd_photo_protection'] = protection
    (out / 'manifest.json').write_text(json.dumps(manifest))
    monkeypatch.setattr(urllib.request, 'urlopen', lambda *a, **kw:
                        io.BytesIO(b'{"station_id":"plant-2"}'))
    with pytest.raises(SystemExit, match='SD photo protection'):
        run_script('flash_ota.py', tmp_path,
                   ['plant-2', '192.168.1.205', '--espota', 'unused.py'], monkeypatch)
