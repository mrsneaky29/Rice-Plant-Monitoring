"""Read provisioning JSON on stdin, store only device-token hashes on the droplet."""
import hashlib
import json
import sqlite3
import sys
import time

payload = json.load(sys.stdin)
tokens = payload['device_tokens']
if set(tokens) != {'plant-1', 'plant-2', 'plant-3'}:
    raise SystemExit('Exactly three station credentials are required')
connection = sqlite3.connect('/var/lib/rice-cloud/rice-cloud.sqlite3')
try:
    with connection:
        for station, token in tokens.items():
            if not isinstance(token, str) or not token.startswith('rcd_') or len(token) < 40:
                raise SystemExit('Invalid device credential')
            digest = hashlib.sha256(token.encode()).hexdigest()
            connection.execute('INSERT OR IGNORE INTO device_tokens(token_hash,station_id,created_at) VALUES(?,?,?)', (digest, station, int(time.time())))
finally:
    connection.close()
print('Three station credentials provisioned; plaintext credentials were not stored on the server.')
