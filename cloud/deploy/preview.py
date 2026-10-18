"""Isolated loopback-only QA server. Never uses production data or credentials."""
import hashlib
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
os.environ['RICE_DATA_DIR'] = str(ROOT / '.private' / 'preview-data')
os.environ['RICE_COOKIE_SECURE'] = 'false'
os.environ['RICE_PUBLIC_ORIGIN'] = 'http://127.0.0.1:8796'
salt = b'isolated-preview-salt-2026'
digest = hashlib.pbkdf2_hmac('sha256', b'Local-preview-only-2026', salt, 600000)
os.environ['RICE_ADMIN_PASSWORD_HASH'] = f'pbkdf2_sha256$600000${salt.hex()}${digest.hex()}'
sys.path.insert(0, str(ROOT / 'server'))
if __name__ == '__main__':
    import uvicorn
    uvicorn.run('app:app', host='127.0.0.1', port=8796)
