"""Interactive local-only provisioning. Never prints secrets or accepts them as arguments."""
import getpass
import hashlib
import json
from pathlib import Path
import secrets
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
PRIVATE = ROOT / '.private'


def configure() -> None:
    PRIVATE.mkdir(exist_ok=True)
    config = PRIVATE / 'provision.json'
    if config.exists():
        print('Private configuration already exists; it was not replaced.')
        return
    password = getpass.getpass('Choose the fixed rice dashboard password (at least 12 characters): ')
    confirm = getpass.getpass('Repeat dashboard password: ')
    if password != confirm or len(password) < 12:
        raise SystemExit('Passwords must match and contain at least 12 characters. Nothing was saved.')
    salt = secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac('sha256', password.encode(), salt, 600000)
    password_hash = f'pbkdf2_sha256$600000${salt.hex()}${digest.hex()}'
    # Obtain the public CA from its authoritative publisher, with verified HTTPS.
    with urllib.request.urlopen('https://letsencrypt.org/certs/isrgrootx1.pem', timeout=30) as response:
        ca = response.read().decode('ascii')
    if 'BEGIN CERTIFICATE' not in ca:
        raise RuntimeError('CA download did not return a PEM certificate')
    tokens = {f'plant-{number}': 'rcd_' + secrets.token_urlsafe(36) for number in range(1, 4)}
    payload = {'password_hash': password_hash, 'device_tokens': tokens}
    config.write_text(json.dumps(payload, indent=2), encoding='utf-8')
    (PRIVATE / 'server.env').write_text(
        'RICE_ADMIN_PASSWORD_HASH=' + password_hash + '\n'
        'RICE_DATA_DIR=/var/lib/rice-cloud\n'
        'RICE_PUBLIC_ORIGIN=https://admin.riceplant.achantalabs.com\n'
        'RICE_COOKIE_SECURE=true\n', encoding='utf-8')
    for station, token in tokens.items():
        path = PRIVATE / station
        path.mkdir(exist_ok=True)
        (path / 'cloud_secrets.h').write_text(
            '#pragma once\n#define CLOUD_DEVICE_TOKEN ' + json.dumps(token) + '\n'
            '#define CLOUD_ROOT_CA ' + json.dumps(ca) + '\n', encoding='utf-8')
    print('Private dashboard and three camera configurations saved locally. No secrets were printed.')


if __name__ == '__main__':
    configure()
