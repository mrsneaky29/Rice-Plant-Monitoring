"""Reuse the existing local OpenAI key privately and verify requested model access."""
import argparse
import getpass
import json
from pathlib import Path
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MODEL = 'gpt-6.1-sol'

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prompt-key', action='store_true', help='Enter a key privately instead of reusing a saved key.')
    args = parser.parse_args()
    key_file = ROOT.parent / 'Three_ESP_Monitoring' / 'PC_Hub' / 'data' / 'openai_key.txt'
    key = key_file.read_text(encoding='utf-8').strip() if key_file.is_file() and not args.prompt_key else getpass.getpass('Paste your OpenAI API key (hidden), then press Enter: ').strip()
    if not key or '\n' in key or '\r' in key:
        raise SystemExit('A valid local API key is required; nothing was enabled.')
    request = urllib.request.Request(f'https://api.openai.com/v1/models/{MODEL}', headers={'Authorization': f'Bearer {key}'})
    try:
        with urllib.request.urlopen(request, timeout=25) as response:
            model = json.load(response)
    except urllib.error.HTTPError as exc:
        raise SystemExit(f'Model access verification failed (HTTP {exc.code}); analysis remains disabled.') from None
    if model.get('id') != MODEL:
        raise SystemExit('The API did not confirm the requested model; analysis remains disabled.')
    env = ROOT / '.private' / 'server.env'
    lines = env.read_text(encoding='utf-8').splitlines()
    names = {'RICE_OPENAI_API_KEY', 'RICE_ANALYSIS_ENABLED', 'RICE_ANALYSIS_MODEL', 'RICE_ANALYSIS_PASSES'}
    lines = [line for line in lines if line.partition('=')[0] not in names]
    lines.extend(['RICE_OPENAI_API_KEY=' + key, 'RICE_ANALYSIS_ENABLED=true', 'RICE_ANALYSIS_MODEL=' + MODEL, 'RICE_ANALYSIS_PASSES=1'])
    env.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'Confirmed account access to {MODEL}. Private analysis configuration prepared; no analysis request was made.')
