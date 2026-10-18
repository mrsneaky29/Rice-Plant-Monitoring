"""Read cloud/capture diagnostics, excluding saved network names and credentials."""
import argparse
import json
import urllib.request

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('address')
    args = parser.parse_args()
    with urllib.request.urlopen(f'http://{args.address}/status', timeout=25) as response:
        status = json.load(response)
    keys = ['station_id', 'photo_count', 'camera_ok', 'sd_mounted', 'cloud_enabled', 'sleep_enabled',
            'maintenance_remaining_seconds', 'upload_error_count', 'last_upload_error', 'time_set', 'time', 'uptime_s']
    print(json.dumps({key: status.get(key) for key in keys}))
    with urllib.request.urlopen(f'http://{args.address}/log?lines=80', timeout=25) as response:
        log = response.read(32768).decode('utf-8', 'replace')
    for line in log.splitlines():
        if '[cloud]' in line or 'Boot: free heap' in line or '[timer]' in line:
            print(line)
