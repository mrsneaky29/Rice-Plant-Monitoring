# Setup and troubleshooting

## Firmware

The sketch includes `secrets.h`. Copy the example before compiling; never commit your credentials. The default camera configuration selects AI Thinker pins and OV5640. Verify the installed module before changing sensor-specific settings.

With Arduino CLI and the ESP32 core installed:

```powershell
arduino-cli lib install "ArduinoJson@7.4.3"
arduino-cli compile --fqbn esp32:esp32:esp32cam --output-dir build-firmware ESP32_Station
```

The local sketch partition table provides two application slots, each `0x1e0000` bytes. Both initial serial flashing and subsequent OTA images must fit those slots. Do not infer OTA fit from a generic IDE maximum alone.

Open the ESP's status page by its current IP, then verify `camera_ok`, `sd_mounted`, `station_id`, `sensor_role`, `timer_enabled`, and `timer_interval`. Camera-only is represented as `none` on the firmware and `camera` in the hub. Enable hourly recording using the firmware page or `/timer?enable=1&interval=3600`. Assign identities through the hub; firmware `/device/config` requires `/reboot?confirm=YES` afterward.

## PC

The runtime is standard-library Python. To produce a Windows executable on Windows:

```powershell
py -3 -m pip install -r requirements-dev.txt
py -3 -m PyInstaller --onefile --noconsole --name Rice_Plant_Monitoring_Hub --add-data "PC_Hub/static;static" --distpath PC_Hub PC_Hub/Monitoring_Hub.py
```

Use `PC_Hub/Open Monitoring Hub.bat`. For login startup, create a Windows Startup shortcut pointing to the executable with working directory `PC_Hub`; omit `--open-browser` if a browser tab should not open at every login. A running hub hosts HTTP 8765 and listens for discovery on UDP 4210.

## Common failures

| Symptom                                 | Check                                                                                                                                        |
| --------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| Executable appears to do nothing        | Open `http://localhost:8765`; the program runs in the background. Use the launcher for automatic browser opening.                            |
| ESP offline after a restart             | Check power and Wi-Fi; DHCP may assign a different IP. Discovery should update it; inspect `/status` at the new address.                     |
| Two cards appear to represent one board | Compare hardware addresses and restore distinct station numbers.                                                                             |
| `/latest.jpg` returns 404               | Take a photo first; the in-memory latest frame does not survive reboot.                                                                      |
| Serial monitor empty after OTA          | OTA is network flashing, not a serial connection. Use status/log endpoints or connect a serial programmer.                                   |
| Board has SD but no hourly photos       | Verify timer enabled, interval 3600, capture window, SD capacity, and clock.                                                                 |
| Dashboard visible only on host          | Ensure same LAN, correct PC IP, and private-network firewall permission.                                                                     |
| API jobs fail                           | Verify account key, configured model identifier, reasoning support, and the visible queue error. Capture still works without API processing. |

Physically connected sensors may remain attached in Camera-only mode, but power and GPIO conflicts can still affect a board. Disconnect peripheral connections that share serial-programming pins before using a programmer.
