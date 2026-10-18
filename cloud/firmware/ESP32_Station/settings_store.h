#ifndef SETTINGS_STORE_H
#define SETTINGS_STORE_H

#include <stdint.h>
#include <stddef.h>

// Everything here lives in the ESP32's internal NVS flash (via
// Preferences), never on the SD card -- the card is photos only.

#define WIFI_MAX_NETWORKS 5
#define WIFI_SSID_MAX 33
#define WIFI_PASS_MAX 65

struct WifiNetwork {
  char ssid[WIFI_SSID_MAX];
  char pass[WIFI_PASS_MAX];
};

enum StationRole : uint8_t {
  STATION_ROLE_NONE = 0,
  STATION_ROLE_SOIL = 1,
  STATION_ROLE_DHT22 = 2
};

void settingsBegin();

// Saved WiFi networks in connection-priority order. Index 0 is the default;
// adding a new network appends it as a fallback. Adding an existing SSID
// updates its password in place; a full list evicts the lowest-priority entry.
int settingsWifiCount();
bool settingsWifiGet(int index, WifiNetwork *out);
bool settingsWifiAdd(const char *ssid, const char *pass);
bool settingsWifiSetPrimary(const char *ssid, const char *pass);
bool settingsWifiDelete(const char *ssid);
void settingsWifiSeedIfEmpty(const char *ssid, const char *pass);

bool settingsFlashEnabled();
void settingsSetFlashEnabled(bool enabled);

bool settingsTimerEnabled();
uint32_t settingsTimerInterval();
bool settingsWindowEnabled();
uint16_t settingsWindowStart();
uint16_t settingsWindowEnd();
void settingsSaveTimer(bool enabled, uint32_t intervalSeconds, bool windowEnabled, uint16_t windowStart, uint16_t windowEnd);

// Same shape as the capture timer settings above, for the DHT22 schedule.
bool settingsDhtTimerEnabled();
uint32_t settingsDhtTimerInterval();
bool settingsDhtWindowEnabled();
uint16_t settingsDhtWindowStart();
uint16_t settingsDhtWindowEnd();
void settingsSaveDhtTimer(bool enabled, uint32_t intervalSeconds, bool windowEnabled, uint16_t windowStart, uint16_t windowEnd);

// Whether AI vision analysis runs on newly captured photos (any capture
// path -- manual, scheduled, live-view snap), not a schedule of its own.
bool settingsAiVisionEnabled();
void settingsSetAiVisionEnabled(bool enabled);

// 128 was too small: OpenAI's newer project-scoped keys ("sk-proj-...")
// commonly run 150-200+ characters, well past the old classic "sk-..."
// key length this was originally sized for. get_query_str()'s underlying
// httpd_query_key_value() fails (not truncates) when the destination is
// too small, which is why pasting a real key produced "key_required" --
// it never got copied out of the query string at all.
#define OPENAI_API_KEY_MAX 300

// The OpenAI API key is never read back out to any HTTP response -- only
// whether one is set (settingsOpenAiKeySet()) is exposed to the web UI.
bool settingsOpenAiKeySet();
bool settingsGetOpenAiKey(char *out, size_t outSize);
void settingsSetOpenAiKey(const char *key);

int settingsStationNumber();
StationRole settingsStationRole();
void settingsStationName(char *out, size_t outSize);
bool settingsSetStationConfig(int stationNumber, StationRole role, const char *name);
const char *settingsStationRoleName(StationRole role);

#endif  // SETTINGS_STORE_H
