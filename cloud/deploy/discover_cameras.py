"""Ask the existing rice camera discovery protocol for LAN identities."""
import json
import socket
import time

if __name__ == '__main__':
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
        connection.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        connection.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        connection.bind(('', 4210))
        connection.settimeout(1)
        connection.sendto(b'{"type":"PC_DISCOVER","service":"rice-monitor","port":8765}', ('255.255.255.255', 4210))
        deadline = time.monotonic() + 8
        seen = set()
        while time.monotonic() < deadline:
            try:
                packet, address = connection.recvfrom(4096)
                station = json.loads(packet)
                identity = station.get('station_id')
                if identity in {'plant-1', 'plant-2', 'plant-3'} and (identity, address[0]) not in seen:
                    seen.add((identity, address[0]))
                    print(identity, address[0])
            except (socket.timeout, ValueError):
                continue
        if not seen:
            print('No camera discovery announcements received.')
