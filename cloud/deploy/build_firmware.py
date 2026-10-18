"""Build station-bound OTA images privately; credentials never enter source packages."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'firmware' / 'ESP32_Station'

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('station', choices=['plant-1', 'plant-2', 'plant-3'])
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--status-only', action='store_true', help='Build connection diagnostics with all photo uploads and cloud command execution disabled.')
    parser.add_argument('--upload-existing-photos', action='store_true', help='Explicitly opt in to uploading older SD photos; default uploads new captures only.')
    args = parser.parse_args()
    if args.status_only and args.upload_existing_photos:
        parser.error('Status-only firmware cannot upload existing photos.')
    # Build stations sequentially through one sketch/cache; only the private
    # cloud header changes, so the ESP32 core and libraries are compiled once.
    stage = ROOT / '.private' / 'build' / 'ESP32_Station'
    stage.mkdir(parents=True, exist_ok=True)
    for path in SOURCE.iterdir():
        if path.is_file() and path.suffix in {'.ino', '.cpp', '.h', '.csv'} and path.name != 'cloud_secrets.h':
            shutil.copy2(path, stage / path.name)
    shutil.copy2(ROOT / '.private' / args.station / 'cloud_secrets.h', stage / 'cloud_secrets.h')
    if not (stage / 'secrets.h').is_file():
        raise SystemExit('Configure local secrets.h with Wi-Fi and OTA credentials before building.')
    (stage / 'cloud_run_mode.h').write_text(
        f'#pragma once\n#define RICE_CLOUD_STATUS_ONLY {int(args.status_only)}\n'
        f'#define RICE_CLOUD_UPLOAD_EXISTING_PHOTOS {int(args.upload_existing_photos)}\n'
        '#define RICE_SD_PROTECT_PHOTOS 1\n', encoding='utf-8')
    output = ROOT / '.private' / 'images' / args.station
    if args.status_only:
        output = output / 'status-only'
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(args.cli), 'compile', '--fqbn', 'esp32:esp32:esp32cam',
                    '--build-path', str(stage.parent / 'cache'), '--output-dir', str(output), str(stage)], check=True)
    image = output / 'ESP32_Station.ino.bin'
    if image.stat().st_size > 0x1e0000:
        raise SystemExit('Image exceeds an OTA partition; do not flash it.')
    (output / 'manifest.json').write_text(json.dumps({'station_id': args.station, 'image': image.name,
        'mode': 'status-only' if args.status_only else 'normal',
        'upload_existing_photos': args.upload_existing_photos,
        'sd_photo_protection': True,
        'bytes': image.stat().st_size, 'sha256': hashlib.sha256(image.read_bytes()).hexdigest()}, indent=2), encoding='utf-8')
    print(f'{args.station} ({"status-only" if args.status_only else "normal"}): OTA image fits the 1,966,080-byte application slot ({image.stat().st_size:,} bytes).')
