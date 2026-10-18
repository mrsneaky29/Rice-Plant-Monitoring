"""Read-only LAN camera readiness check; prints no Wi-Fi/OTA credentials."""
from concurrent.futures import ThreadPoolExecutor
import argparse
import json
import urllib.request


def check(host):
    try:
        with urllib.request.urlopen(f'http://{host}/status', timeout=4) as response:
            data = json.load(response)
        keys = ['station_id', 'mac', 'camera_ok', 'sd_mounted', 'timer_enabled', 'timer_interval', 'photo_count', 'cloud_enabled', 'sleep_enabled', 'maintenance_remaining_seconds', 'upload_error_count', 'last_upload_error', 'time_set', 'uptime_s']
        print(host, {key: data.get(key) for key in keys})
    except Exception as exc:
        print(host, type(exc).__name__)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('addresses', nargs='*', default=['192.168.1.203', '192.168.1.204', '192.168.1.205'])
    args = parser.parse_args()
    with ThreadPoolExecutor(3) as pool:
        list(pool.map(check, args.addresses))
