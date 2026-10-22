"""Enabled-analysis startup and queue checks using synthetic data and no provider calls."""
import hashlib
import importlib
import importlib.util
import os
from pathlib import Path
import sys

from fastapi.testclient import TestClient


def test_enabled_analysis_startup_new_upload_and_archive_exclusion(monkeypatch, tmp_path):
    root = Path(__file__).resolve().parents[1]
    source = Path(os.environ.get('RICE_ANALYSIS_TEST_APP', root / 'server/app.py'))
    monkeypatch.syspath_prepend(str(root / 'server'))
    monkeypatch.setenv('RICE_DATA_DIR', str(tmp_path / 'data'))
    monkeypatch.setenv('RICE_DB_PATH', str(tmp_path / 'data/test.sqlite3'))
    monkeypatch.setenv('RICE_ANALYSIS_ENABLED', 'true')
    monkeypatch.setenv('RICE_ANALYSIS_MODEL', 'gpt-6.1-sol')
    monkeypatch.setenv('RICE_ANALYSIS_PASSES', '1')
    monkeypatch.setenv('RICE_OPENAI_API_KEY', 'synthetic-test-key')
    salt = b'0123456789abcdef'
    password = hashlib.pbkdf2_hmac('sha256', b'synthetic-password', salt, 200000).hex()
    monkeypatch.setenv('RICE_ADMIN_PASSWORD_HASH', f'pbkdf2_sha256$200000${salt.hex()}${password}')
    monkeypatch.setenv('RICE_COOKIE_SECURE', 'false')
    monkeypatch.delenv('RICE_PUBLIC_ORIGIN', raising=False)
    spec = importlib.util.spec_from_file_location('app', source)
    module = importlib.util.module_from_spec(spec)
    monkeypatch.setitem(sys.modules, 'app', module)
    spec.loader.exec_module(module)
    worker = importlib.reload(importlib.import_module('analysis_worker'))
    monkeypatch.setattr(worker, 'start_worker', lambda: None)
    monkeypatch.setattr(worker, 'stop_worker', lambda: None)
    def reject_provider_call(*args, **kwargs):
        raise AssertionError('Synthetic tests must never contact the provider')
    monkeypatch.setattr(worker, '_http', reject_provider_call)
    with TestClient(module.app) as client:
        assert client.get('/api/health').status_code == 200
        with module.db() as conn:
            conn.execute('INSERT INTO device_tokens(token_hash,station_id,created_at) VALUES(?,?,1)', (module.token_hash('synthetic-device-token'), 'plant-2'))
        photo = b'\xff\xd8\xffsynthetic\xff\xd9'
        headers = {'Authorization': 'Bearer synthetic-device-token', 'Content-Type': 'image/jpeg', 'X-Content-SHA256': hashlib.sha256(photo).hexdigest()}
        url = '/api/device/photos/2026-10-22/synthetic.jpg'
        assert client.put(url, content=photo, headers=headers).status_code == 200
        assert client.put(url, content=photo, headers=headers).json()['duplicate'] is True
        archive_headers = {**headers, 'X-Rice-Archive-Import': 'true'}
        assert client.put('/api/device/photos/2026-10-21/archive.jpg', content=photo, headers=archive_headers).status_code == 200
        with module.db() as conn:
            assert conn.execute('SELECT count(*) FROM photos').fetchone()[0] == 2
            assert conn.execute('SELECT count(*) FROM analysis_jobs').fetchone()[0] == 1
    with TestClient(module.app):
        with module.db() as conn:
            assert conn.execute('SELECT count(*) FROM analysis_jobs').fetchone()[0] == 1
