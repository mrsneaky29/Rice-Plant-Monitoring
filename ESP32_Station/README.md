# Three-ESP station firmware

This is the shared build for all three AI Thinker ESP32-CAM stations. It is a
separate copy; the original firmware projects were not modified.

## Profiles

- Plant 1, Plant 2, and Plant 3: `none` — Camera-only in the PC dashboard.
- Soil and DHT22 modules are retained as earlier experimental code but disabled in this deployment.

The station number, display name, role, OTA hostname, and web hostname persist
after restart. Use the PC dashboard's Device setup section to configure each
board by its current IP address. The unique hostnames are `ricecam-1.local`,
`ricecam-2.local`, and `ricecam-3.local`.

## Offline behavior

Camera timers do not depend on the PC. If the PC does not respond,
the station continues saving photographs to SD. Sensor recording is disabled.
It sends no OpenAI request. When the PC returns, UDP discovery
causes the PC to retrieve the backlog. Files remain on SD indefinitely.

## Build

- Sketch: `ESP32_Station.ino`
- Board: **AI Thinker ESP32-CAM**
- Flash mode: **DIO**
- Partition scheme: use the project's `partitions.csv` (two OTA app slots)
- Library: **ArduinoJson 7.4.3**
- Copy `secrets.example.h` to `secrets.h` before compiling and fill in your credentials.

## Wiring

Plant 1 uses GPIO3/RX0 as ADS1115 SDA and GPIO1/TX0 as SCL. Disconnect those
lines while using a serial programmer. Plant 2 uses GPIO13 for DHT22 data.
Plant 3 has no external sensor.

The on-device OpenAI feature is forced off. Analysis is performed by the
Windows hub after photographs have been copied safely to the PC.
