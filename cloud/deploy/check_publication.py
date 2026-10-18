"""Fail if locally configured secrets occur in a public-source candidate."""
import ast
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'deploy'))
from package import sources


if __name__ == '__main__':
    values = []
    header = ROOT / 'firmware' / 'ESP32_Station' / 'secrets.h'
    if header.exists():
        text = header.read_text(encoding='utf-8')
        for literal in re.findall(r'^\s*#define\s+RICE_\w+\s+("(?:[^"\\]|\\.)*")', text, re.MULTILINE):
            value = ast.literal_eval(literal)
            if value and len(value) >= 4:
                values.append(value.encode())
    private = ROOT / '.private' / 'provision.json'
    if private.exists():
        payload = json.loads(private.read_text(encoding='utf-8'))
        values.extend(token.encode() for token in payload['device_tokens'].values())
        values.append(payload['password_hash'].encode())
    environment = ROOT / '.private' / 'server.env'
    if environment.exists():
        for line in environment.read_text(encoding='utf-8').splitlines():
            name, _, value = line.partition('=')
            if name == 'RICE_OPENAI_API_KEY' and value:
                values.append(value.encode())
    failures = []
    count = 0
    for path, relative in sources():
        contents = path.read_bytes()
        count += 1
        if any(value in contents for value in values):
            failures.append(str(relative))
    if failures:
        raise SystemExit('Configured secret found in public candidate: ' + ', '.join(failures))
    print(f'Publication check passed: {count} source files; private values absent.')
