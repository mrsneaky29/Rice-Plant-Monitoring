"""Copy existing deployment network/OTA settings into the ignored cloud firmware header."""
import ast
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL = ROOT.parent / 'Three_ESP_Monitoring' / 'ESP32_Station' / 'ESP32_Station.ino'
NAMES = {'PRIMARY_WIFI_SSID': 'RICE_WIFI_SSID', 'PRIMARY_WIFI_PASSWORD': 'RICE_WIFI_PASSWORD',
         'FALLBACK_WIFI_SSID': 'RICE_FALLBACK_SSID', 'FALLBACK_WIFI_PASSWORD': 'RICE_FALLBACK_PASSWORD',
         'OTA_PASSWORD': 'RICE_OTA_PASSWORD'}

if __name__ == '__main__':
    source = ORIGINAL.read_text(encoding='utf-8')
    values = {}
    for old, new in NAMES.items():
        match = re.search(r'static\s+const\s+char\s*\*\s*' + re.escape(old) + r'\s*=\s*("(?:[^"\\]|\\.)*")', source)
        if not match:
            raise SystemExit('Could not recover every existing network setting. Configure secrets.h locally instead.')
        values[new] = ast.literal_eval(match.group(1))
    target = ROOT / 'firmware' / 'ESP32_Station' / 'secrets.h'
    content = '#pragma once\n' + ''.join(f'#define {name} {json.dumps(value)}\n' for name, value in values.items())
    target.write_text(content, encoding='utf-8')
    print('Existing network/OTA settings copied privately; original firmware unchanged.')
