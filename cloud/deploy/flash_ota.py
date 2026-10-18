"""Station-checked local OTA. Reads the existing OTA password privately, never argv."""
import argparse
import ast
import hashlib
import importlib.util
import ipaddress
import json
from pathlib import Path
import re
import socket
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def verify_discovery_identity(address, station, expected_mac):
    """Low-memory diagnostic alternative when the device HTTP server is stalled."""
    wanted = expected_mac.replace(':', '').replace('-', '').upper()
    if not re.fullmatch(r'[0-9A-F]{12}', wanted):
        raise SystemExit('An exact expected hardware MAC is required. Nothing was flashed.')
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        connection.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        connection.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        connection.bind(('', 4210))
        connection.settimeout(1)
        connection.sendto(b'{"type":"PC_DISCOVER"}', (str(address), 4210))
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            try:
                packet, peer = connection.recvfrom(4096)
                if peer[0] != str(address):
                    continue
                status = json.loads(packet)
                if status.get('station_id') != station:
                    raise SystemExit('Station identity mismatch. Nothing was flashed.')
                actual = str(status.get('mac', '')).replace(':', '').replace('-', '').upper()
                if actual != wanted:
                    raise SystemExit('Hardware MAC mismatch. Nothing was flashed.')
                return
            except (socket.timeout, ValueError):
                continue
    raise SystemExit('No matching diagnostic discovery reply. Nothing was flashed.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('station', choices=['plant-1', 'plant-2', 'plant-3'])
    parser.add_argument('address', type=ipaddress.IPv4Address)
    parser.add_argument('--espota', type=Path, required=True)
    parser.add_argument('--status-only', action='store_true', help='Select the separate connection-only image; photo uploads remain disabled.')
    parser.add_argument('--discovery-identity', action='store_true', help='For status-only recovery, verify station and exact hardware MAC over discovery instead of HTTP.')
    parser.add_argument('--expected-mac', default='')
    parser.add_argument('--upload-existing-photos', action='store_true', help='Confirm an explicitly built image that uploads older SD photos.')
    args = parser.parse_args()
    if not args.address.is_private:
        raise SystemExit('Local Wi-Fi OTA requires a private LAN address.')
    if args.discovery_identity:
        if not args.status_only:
            raise SystemExit('Discovery recovery is restricted to status-only images. Nothing was flashed.')
        verify_discovery_identity(args.address, args.station, args.expected_mac)
    else:
        with urllib.request.urlopen(f'http://{args.address}/status', timeout=8) as response:
            status = json.load(response)
        if status.get('station_id') != args.station:
            raise SystemExit('Station identity does not match the selected image. Nothing was flashed.')
        if args.expected_mac:
            wanted = args.expected_mac.replace(':', '').replace('-', '').upper()
            actual = str(status.get('mac', '')).replace(':', '').replace('-', '').upper()
            if not re.fullmatch(r'[0-9A-F]{12}', wanted) or actual != wanted:
                raise SystemExit('Hardware MAC mismatch. Nothing was flashed.')
    output = ROOT / '.private' / 'images' / args.station
    if args.status_only:
        output = output / 'status-only'
    manifest = json.loads((output / 'manifest.json').read_text(encoding='utf-8'))
    expected_mode = 'status-only' if args.status_only else 'normal'
    if manifest.get('mode', 'normal') != expected_mode:
        raise SystemExit('Firmware mode does not match the selected image. Nothing was flashed.')
    if not args.status_only and manifest.get('upload_existing_photos') is not args.upload_existing_photos:
        raise SystemExit('Existing-photo upload policy is missing or differs from the requested policy. Rebuild before flashing.')
    if not args.status_only and manifest.get('sd_photo_protection') is not True:
        raise SystemExit('Normal firmware must declare SD photo protection. Rebuild before flashing.')
    image = output / 'ESP32_Station.ino.bin'
    if manifest.get('station_id') != args.station or hashlib.sha256(image.read_bytes()).hexdigest() != manifest.get('sha256'):
        raise SystemExit('Private firmware manifest mismatch. Rebuild before flashing.')
    if image.stat().st_size > 0x1e0000:
        raise SystemExit('Firmware does not fit the OTA partition.')
    secrets_header = (ROOT / 'firmware' / 'ESP32_Station' / 'secrets.h').read_text(encoding='utf-8')
    match = re.search(r'^\s*#define\s+RICE_OTA_PASSWORD\s+("(?:[^"\\]|\\.)*")', secrets_header, re.MULTILINE)
    if not match:
        raise SystemExit('Could not load the existing OTA password from local secrets.h.')
    password = ast.literal_eval(match.group(1))
    spec = importlib.util.spec_from_file_location('rice_espota', args.espota)
    ota = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ota)
    ota.PROGRESS = False
    ota.TIMEOUT = 20
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
        reservation.bind(('', 0))
        local_port = reservation.getsockname()[1]
    result = ota.serve(str(args.address), '0.0.0.0', 3232, local_port, password, False, str(image))
    if result:
        raise SystemExit('OTA failed; verify the existing firmware is reachable before retrying.')
    print(f'{args.station}: OTA transfer completed. Verify reboot, camera, SD, cloud connection and sleep separately.')
