"""Diagnostic OTA identity checks with fake UDP, no devices or credentials."""
import importlib.util
import ipaddress
import json
from pathlib import Path

import pytest


SPEC = importlib.util.spec_from_file_location(
    'ota_identity_recovery', Path(__file__).resolve().parents[1] / 'deploy' / 'flash_ota.py')
ota = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ota)
MAC = '14:2B:2F:C7:3A:2C'
IP = '192.168.1.204'


@pytest.mark.parametrize('station,mac,peer,error', [
    ('plant-2', MAC, IP, None),
    ('plant-1', MAC, IP, 'Station identity mismatch'),
    ('plant-2', 'AA:BB:CC:DD:EE:FF', IP, 'Hardware MAC mismatch'),
    ('plant-2', MAC, '192.168.1.205', 'No matching diagnostic'),
])
def test_discovery_requires_target_ip_station_and_hardware_mac(monkeypatch, station, mac, peer, error):
    calls = []

    class FakeSocket:
        def __enter__(self): return self
        def __exit__(self, *args): pass
        def setsockopt(self, *args): pass
        def bind(self, *args): pass
        def settimeout(self, *args): pass
        def sendto(self, packet, target): calls.append(target)
        def recvfrom(self, size):
            return json.dumps({'station_id': station, 'mac': mac}).encode(), (peer, 4210)

    ticks = iter([0, 1, 20])
    monkeypatch.setattr(ota.socket, 'socket', lambda *args: FakeSocket())
    monkeypatch.setattr(ota.time, 'monotonic', lambda: next(ticks))
    if error:
        with pytest.raises(SystemExit, match=error):
            ota.verify_discovery_identity(ipaddress.IPv4Address(IP), 'plant-2', MAC)
    else:
        ota.verify_discovery_identity(ipaddress.IPv4Address(IP), 'plant-2', MAC)
    assert calls == [(IP, 4210)]


def test_missing_expected_mac_fails_before_network(monkeypatch):
    def forbidden(*args):
        pytest.fail('An invalid hardware identity must not open a socket')
    monkeypatch.setattr(ota.socket, 'socket', forbidden)
    with pytest.raises(SystemExit, match='exact expected hardware MAC'):
        ota.verify_discovery_identity(ipaddress.IPv4Address(IP), 'plant-2', '')
