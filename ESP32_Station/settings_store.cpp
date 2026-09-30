#include "settings_store.h"
#include <Preferences.h>

static const char *NS = "esp32cam";

void settingsBegin() {
  // Each accessor opens/closes its own Preferences handle -- these calls
  // are infrequent (user-triggered settings changes, not per-request), so
  // there's no need to hold NVS open for the sketch's lifetime.
  // Apply the temporary camera-only rollout once without erasing WiFi,
  // station identity, schedules, or any files on the SD card.
  Preferences p;
  p.begin(NS, false);
  if (!p.getBool("cameraOnlyV1", false)) {
    p.putUChar("stationRole", (uint8_t)STATION_ROLE_NONE);
    p.putBool("cameraOnlyV1", true);
  }
  p.end();
}

static void keyFor(char *buf, size_t bufSize, char prefix, int index) {
  snprintf(buf, bufSize, "%c%d", prefix, index);
}

int settingsWifiCount() {
  Preferences p;
  p.begin(NS, true);
  int n = p.getInt("wifiN", 0);
  p.end();
  if (n < 0) n = 0;
  if (n > WIFI_MAX_NETWORKS) n = WIFI_MAX_NETWORKS;
  return n;
}

bool settingsWifiGet(int index, WifiNetwork *out) {
  int n = settingsWifiCount();
  if (index < 0 || index >= n || !out) {
    return false;
  }
  Preferences p;
  p.begin(NS, true);
  char ks[8], kp[8];
  keyFor(ks, sizeof(ks), 's', index);
  keyFor(kp, sizeof(kp), 'p', index);
  String ssid = p.getString(ks, "");
  String pass = p.getString(kp, "");
  p.end();
  snprintf(out->ssid, sizeof(out->ssid), "%s", ssid.c_str());
  snprintf(out->pass, sizeof(out->pass), "%s", pass.c_str());
  return true;
}

bool settingsWifiAdd(const char *ssid, const char *pass) {
  if (!ssid || !ssid[0] || strlen(ssid) >= WIFI_SSID_MAX || (pass && strlen(pass) >= WIFI_PASS_MAX)) {
    return false;
  }
  Preferences p;
  p.begin(NS, false);
  int n = p.getInt("wifiN", 0);
  if (n < 0) n = 0;
  if (n > WIFI_MAX_NETWORKS) n = WIFI_MAX_NETWORKS;

  int foundIdx = -1;
  for (int i = 0; i < n; i++) {
    char ks[8];
    keyFor(ks, sizeof(ks), 's', i);
    if (p.getString(ks, "") == String(ssid)) {
      foundIdx = i;
      break;
    }
  }

  int idx;
  if (foundIdx >= 0) {
    idx = foundIdx;
  } else if (n < WIFI_MAX_NETWORKS) {
    idx = n;
    n++;
  } else {
    // Full: replace the lowest-priority fallback while preserving index 0.
    idx = WIFI_MAX_NETWORKS - 1;
  }

  char ks[8], kp[8];
  keyFor(ks, sizeof(ks), 's', idx);
  keyFor(kp, sizeof(kp), 'p', idx);
  p.putString(ks, ssid);
  p.putString(kp, pass ? pass : "");
  p.putInt("wifiN", n);
  p.end();
  return true;
}

bool settingsWifiSetPrimary(const char *ssid, const char *pass) {
  if (!ssid || !ssid[0] || strlen(ssid) >= WIFI_SSID_MAX || (pass && strlen(pass) >= WIFI_PASS_MAX)) {
    return false;
  }

  Preferences p;
  p.begin(NS, false);
  int n = p.getInt("wifiN", 0);
  if (n < 0) n = 0;
  if (n > WIFI_MAX_NETWORKS) n = WIFI_MAX_NETWORKS;

  WifiNetwork existing[WIFI_MAX_NETWORKS];
  int existingIndex = -1;
  for (int i = 0; i < n; i++) {
    char ks[8], kp[8];
    keyFor(ks, sizeof(ks), 's', i);
    keyFor(kp, sizeof(kp), 'p', i);
    snprintf(existing[i].ssid, sizeof(existing[i].ssid), "%s", p.getString(ks, "").c_str());
    snprintf(existing[i].pass, sizeof(existing[i].pass), "%s", p.getString(kp, "").c_str());
    if (strcmp(existing[i].ssid, ssid) == 0) {
      existingIndex = i;
    }
  }

  // Avoid an NVS write on every boot once the primary already matches.
  if (n > 0 && existingIndex == 0 && strcmp(existing[0].pass, pass ? pass : "") == 0) {
    p.end();
    return true;
  }

  char firstSsid[WIFI_SSID_MAX], firstPass[WIFI_PASS_MAX];
  snprintf(firstSsid, sizeof(firstSsid), "%s", ssid);
  snprintf(firstPass, sizeof(firstPass), "%s", pass ? pass : "");

  int newCount = n;
  if (existingIndex < 0 && newCount < WIFI_MAX_NETWORKS) {
    newCount++;
  }
  if (newCount == 0) {
    newCount = 1;
  }

  char ks[8], kp[8];
  keyFor(ks, sizeof(ks), 's', 0);
  keyFor(kp, sizeof(kp), 'p', 0);
  p.putString(ks, firstSsid);
  p.putString(kp, firstPass);

  int dest = 1;
  for (int i = 0; i < n && dest < newCount; i++) {
    if (i == existingIndex) continue;
    keyFor(ks, sizeof(ks), 's', dest);
    keyFor(kp, sizeof(kp), 'p', dest);
    p.putString(ks, existing[i].ssid);
    p.putString(kp, existing[i].pass);
    dest++;
  }

  p.putInt("wifiN", newCount);
  p.end();
  return true;
}

bool settingsWifiDelete(const char *ssid) {
  if (!ssid) {
    return false;
  }
  Preferences p;
  p.begin(NS, false);
  int n = p.getInt("wifiN", 0);
  if (n < 0) n = 0;
  if (n > WIFI_MAX_NETWORKS) n = WIFI_MAX_NETWORKS;

  int foundIdx = -1;
  for (int i = 0; i < n; i++) {
    char ks[8];
    keyFor(ks, sizeof(ks), 's', i);
    if (p.getString(ks, "") == String(ssid)) {
      foundIdx = i;
      break;
    }
  }
  if (foundIdx < 0) {
    p.end();
    return false;
  }

  for (int i = foundIdx; i < n - 1; i++) {
    char ksSrc[8], kpSrc[8], ksDst[8], kpDst[8];
    keyFor(ksSrc, sizeof(ksSrc), 's', i + 1);
    keyFor(kpSrc, sizeof(kpSrc), 'p', i + 1);
    keyFor(ksDst, sizeof(ksDst), 's', i);
    keyFor(kpDst, sizeof(kpDst), 'p', i);
    p.putString(ksDst, p.getString(ksSrc, ""));
    p.putString(kpDst, p.getString(kpSrc, ""));
  }
  char lastS[8], lastP[8];
  keyFor(lastS, sizeof(lastS), 's', n - 1);
  keyFor(lastP, sizeof(lastP), 'p', n - 1);
  p.remove(lastS);
  p.remove(lastP);
  p.putInt("wifiN", n - 1);
  p.end();
  return true;
}

void settingsWifiSeedIfEmpty(const char *ssid, const char *pass) {
  if (settingsWifiCount() > 0) {
    return;
  }
  settingsWifiAdd(ssid, pass);
}

bool settingsFlashEnabled() {
  Preferences p;
  p.begin(NS, true);
  bool v = p.getBool("flashOn", true);
  p.end();
  return v;
}

void settingsSetFlashEnabled(bool enabled) {
  Preferences p;
  p.begin(NS, false);
  p.putBool("flashOn", enabled);
  p.end();
}

bool settingsTimerEnabled() {
  Preferences p;
  p.begin(NS, true);
  bool v = p.getBool("timerOn", false);
  p.end();
  return v;
}

uint32_t settingsTimerInterval() {
  Preferences p;
  p.begin(NS, false);
  uint32_t v = p.getUInt("timerIv", 3600);

  // Earlier builds used a 10-second default. Reflashing does not erase NVS,
  // so migrate that legacy default once; a user-selected interval remains
  // untouched after the marker has been stored.
  if (!p.getBool("timer1hV1", false)) {
    if (v == 10) {
      v = 3600;
      p.putUInt("timerIv", v);
    }
    p.putBool("timer1hV1", true);
  }
  p.end();
  return v ? v : 3600;
}

bool settingsWindowEnabled() {
  Preferences p;
  p.begin(NS, true);
  bool v = p.getBool("winOn", false);
  p.end();
  return v;
}

uint16_t settingsWindowStart() {
  Preferences p;
  p.begin(NS, true);
  uint16_t v = p.getUShort("winSt", 360);
  p.end();
  return v;
}

uint16_t settingsWindowEnd() {
  Preferences p;
  p.begin(NS, true);
  uint16_t v = p.getUShort("winEn", 1200);
  p.end();
  return v;
}

void settingsSaveTimer(bool enabled, uint32_t intervalSeconds, bool windowEnabled, uint16_t windowStart, uint16_t windowEnd) {
  Preferences p;
  p.begin(NS, false);
  p.putBool("timerOn", enabled);
  p.putUInt("timerIv", intervalSeconds);
  p.putBool("winOn", windowEnabled);
  p.putUShort("winSt", windowStart);
  p.putUShort("winEn", windowEnd);
  p.end();
}

bool settingsDhtTimerEnabled() {
  Preferences p;
  p.begin(NS, true);
  bool v = p.getBool("dhtOn", false);
  p.end();
  return v;
}

uint32_t settingsDhtTimerInterval() {
  Preferences p;
  p.begin(NS, true);
  uint32_t v = p.getUInt("dhtIv", 60);
  p.end();
  return v ? v : 60;
}

bool settingsDhtWindowEnabled() {
  Preferences p;
  p.begin(NS, true);
  bool v = p.getBool("dhtWinOn", false);
  p.end();
  return v;
}

uint16_t settingsDhtWindowStart() {
  Preferences p;
  p.begin(NS, true);
  uint16_t v = p.getUShort("dhtWinSt", 0);
  p.end();
  return v;
}

uint16_t settingsDhtWindowEnd() {
  Preferences p;
  p.begin(NS, true);
  uint16_t v = p.getUShort("dhtWinEn", 1439);
  p.end();
  return v;
}

void settingsSaveDhtTimer(bool enabled, uint32_t intervalSeconds, bool windowEnabled, uint16_t windowStart, uint16_t windowEnd) {
  Preferences p;
  p.begin(NS, false);
  p.putBool("dhtOn", enabled);
  p.putUInt("dhtIv", intervalSeconds);
  p.putBool("dhtWinOn", windowEnabled);
  p.putUShort("dhtWinSt", windowStart);
  p.putUShort("dhtWinEn", windowEnd);
  p.end();
}

bool settingsAiVisionEnabled() {
  Preferences p;
  p.begin(NS, true);
  bool v = p.getBool("aiVision", false);
  p.end();
  return v;
}

void settingsSetAiVisionEnabled(bool enabled) {
  Preferences p;
  p.begin(NS, false);
  p.putBool("aiVision", enabled);
  p.end();
}

bool settingsOpenAiKeySet() {
  Preferences p;
  p.begin(NS, true);
  String k = p.getString("oaiKey", "");
  p.end();
  return k.length() > 0;
}

bool settingsGetOpenAiKey(char *out, size_t outSize) {
  Preferences p;
  p.begin(NS, true);
  String k = p.getString("oaiKey", "");
  p.end();
  if (k.length() == 0) {
    if (outSize) out[0] = '\0';
    return false;
  }
  snprintf(out, outSize, "%s", k.c_str());
  return true;
}

void settingsSetOpenAiKey(const char *key) {
  Preferences p;
  p.begin(NS, false);
  p.putString("oaiKey", key ? key : "");
  p.end();
}

int settingsStationNumber() {
  Preferences p;
  p.begin(NS, true);
  int v = p.getUChar("stationNo", 1);
  p.end();
  return (v >= 1 && v <= 3) ? v : 1;
}

StationRole settingsStationRole() {
  Preferences p;
  p.begin(NS, true);
  int v = p.getUChar("stationRole", STATION_ROLE_NONE);
  p.end();
  if (v < STATION_ROLE_NONE || v > STATION_ROLE_DHT22) v = STATION_ROLE_NONE;
  return (StationRole)v;
}

void settingsStationName(char *out, size_t outSize) {
  if (!out || outSize == 0) return;
  Preferences p;
  p.begin(NS, true);
  String value = p.getString("stationName", "");
  p.end();
  if (value.length() == 0) value = "Plant " + String(settingsStationNumber());
  snprintf(out, outSize, "%s", value.c_str());
}

bool settingsSetStationConfig(int stationNumber, StationRole role, const char *name) {
  if (stationNumber < 1 || stationNumber > 3 || role < STATION_ROLE_NONE || role > STATION_ROLE_DHT22) return false;
  if (name && strlen(name) > 31) return false;
  if (name && (strchr(name, '"') || strchr(name, '\\') || strchr(name, '\r') || strchr(name, '\n'))) return false;
  Preferences p;
  p.begin(NS, false);
  p.putUChar("stationNo", (uint8_t)stationNumber);
  p.putUChar("stationRole", (uint8_t)role);
  if (name && name[0]) p.putString("stationName", name);
  p.end();
  return true;
}

const char *settingsStationRoleName(StationRole role) {
  switch (role) {
    case STATION_ROLE_SOIL: return "soil";
    case STATION_ROLE_DHT22: return "dht22";
    default: return "none";
  }
}
