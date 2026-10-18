#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>

#include "board_config.h"
#include "sd_storage.h"
#include "photo_capture.h"
#include "capture_timer.h"
#include "time_keeper.h"
#include "settings_store.h"
#include "device_log.h"
#include "dht_sensor.h"
#include "sensor_timer.h"
#include "water_level.h"
#include "camera_setup.h"
#include "peripheral_bus.h"
#include "ads1115.h"
#include "station_identity.h"
#include "station_discovery.h"
#include "cloud_device.h"
#include "secrets.h"

// Cloud sync, TLS, JSON, and photo hashing run from Arduino loopTask.
SET_LOOP_TASK_STACK_SIZE(32768);

static char stationHostname[24] = "ricecam-1";
// The first saved network is always tried first. Keep the original seed as a
// fallback so existing deployments are not stranded if the new AP is absent.
static const char *PRIMARY_WIFI_SSID = RICE_WIFI_SSID;
static const char *PRIMARY_WIFI_PASSWORD = RICE_WIFI_PASSWORD;
static const char *FALLBACK_WIFI_SSID = RICE_FALLBACK_SSID;
static const char *FALLBACK_WIFI_PASSWORD = RICE_FALLBACK_PASSWORD;
static const char *OTA_PASSWORD = RICE_OTA_PASSWORD;

void startCameraServer();

static const char *wifiStatusName(wl_status_t st) {
  switch (st) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_SCAN_COMPLETED: return "SCAN_DONE";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

static void logWifiConnected(const char *why) {
  deviceLog("WiFi %s: ssid=\"%s\" ip=%s gw=%s rssi=%d dBm ch=%d mac=%s",
            why,
            WiFi.SSID().c_str(),
            WiFi.localIP().toString().c_str(),
            WiFi.gatewayIP().toString().c_str(),
            (int)WiFi.RSSI(),
            (int)WiFi.channel(),
            WiFi.macAddress().c_str());
}

static bool wifiConnectBest(uint32_t timeoutMs) {
  int count = settingsWifiCount();
  if (count == 0) {
    deviceLog("WiFi: no saved networks in NVS");
    return false;
  }

  WifiNetwork nets[WIFI_MAX_NETWORKS];
  for (int i = 0; i < count; i++) {
    settingsWifiGet(i, &nets[i]);
    deviceLog("WiFi saved[%d]: \"%s\"", i, nets[i].ssid);
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  delay(100);

  deviceLog("WiFi scanning (timeout budget %u ms)...", (unsigned)timeoutMs);
  int found = WiFi.scanNetworks();
  if (found < 0) {
    deviceLog("WiFi scan failed (code %d) -- will try saved SSIDs blindly", found);
    found = 0;
  } else {
    deviceLog("WiFi scan found %d AP(s)", found);
  }

  int rssiByIdx[WIFI_MAX_NETWORKS];
  bool seenByIdx[WIFI_MAX_NETWORKS];
  for (int i = 0; i < count; i++) {
    rssiByIdx[i] = -1000;
    seenByIdx[i] = false;
  }

  for (int i = 0; i < found; i++) {
    String seenSsid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    deviceLog("  AP \"%s\" rssi=%d ch=%d", seenSsid.c_str(), rssi, WiFi.channel(i));
    for (int j = 0; j < count; j++) {
      if (seenSsid == nets[j].ssid) {
        seenByIdx[j] = true;
        if (rssi > rssiByIdx[j]) {
          rssiByIdx[j] = rssi;
        }
      }
    }
  }
  WiFi.scanDelete();

  // Do not reorder networks by signal strength: index 0 is the configured
  // default and the remaining saved networks are deliberate fallbacks.
  uint32_t budgetLeft = timeoutMs;
  uint32_t perCandidateMs = count > 1 ? (timeoutMs / count) : timeoutMs;
  if (perCandidateMs < 5000) {
    perCandidateMs = 5000;
  }

  for (int i = 0; i < count && budgetLeft > 0; i++) {
    WifiNetwork *n = &nets[i];
    deviceLog("WiFi trying \"%s\"%s rssi=%d slice=%u ms",
              n->ssid,
              seenByIdx[i] ? "" : " (not in scan)",
              rssiByIdx[i],
              (unsigned)(perCandidateMs < budgetLeft ? perCandidateMs : budgetLeft));
    // Credentials are supplied explicitly below; do not erase the STA state
    // or power down the radio between fallback attempts.
    WiFi.disconnect(false, false);
    delay(50);
    WiFi.begin(n->ssid, n->pass);

    uint32_t slice = perCandidateMs < budgetLeft ? perCandidateMs : budgetLeft;
    uint32_t startMs = millis();
    wl_status_t last = WL_IDLE_STATUS;
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < slice) {
      delay(300);
      wl_status_t st = WiFi.status();
      if (st != last) {
        deviceLog("WiFi status -> %s", wifiStatusName(st));
        last = st;
      }
    }

    uint32_t used = millis() - startMs;
    budgetLeft -= used > budgetLeft ? budgetLeft : used;

    if (WiFi.status() == WL_CONNECTED) {
      logWifiConnected("connected");
      return true;
    }
    deviceLog("WiFi \"%s\" failed: %s after %u ms", n->ssid, wifiStatusName(WiFi.status()), (unsigned)used);
  }

  deviceLog("WiFi: all saved networks failed");
  return false;
}

void setup() {
  Serial.setDebugOutput(false);

  deviceLogBegin();
  settingsBegin();
  cloudDeviceBegin();
  stationIdentityHostname(stationHostname, sizeof(stationHostname));
  settingsWifiSeedIfEmpty(FALLBACK_WIFI_SSID, FALLBACK_WIFI_PASSWORD);
  if (!settingsWifiSetPrimary(PRIMARY_WIFI_SSID, PRIMARY_WIFI_PASSWORD)) {
    deviceLog("WiFi: could not save configured default network");
  }
  deviceLog("Boot: free heap=%u PSRAM=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

  bool sdOk = sdStorageBegin();
  if (!sdOk) {
    deviceLog("Continuing without SD card");
  }

  bool cameraOk = cameraSetupBegin();
  if (!cameraOk) {
    deviceLog("Camera init failed -- continuing without it");
  }
  photoCaptureSetCameraOk(cameraOk);

  if (cameraOk) {
    photoCaptureSetMaxFramesize(cameraSetupMaxFramesize());
    photoCaptureSetStreamFramesize(cameraSetupStreamFramesize());
    photoCaptureSetQuality(cameraSetupStreamQuality(), cameraSetupCaptureQuality());
    photoCaptureSetStreamMinIntervalMs(cameraSetupStreamMinIntervalMs());
    photoCaptureSyncIdleMode();
  }

  photoCaptureBegin();
  if (settingsStationRole() == STATION_ROLE_SOIL) {
    peripheralBusBegin();
    ads1115Begin();
  }
  captureTimerBegin();
  dhtSensorBegin();
  sensorTimerBegin();
  waterLevelBegin();

  photoCaptureSetFlashEnabled(settingsFlashEnabled());
  captureTimerRestoreSchedule(settingsTimerEnabled(), settingsTimerInterval(),
                              settingsWindowEnabled(), settingsWindowStart(),
                              settingsWindowEnd());
  sensorTimerSetIntervalSeconds(1200);
  sensorTimerSetWindow(settingsDhtWindowEnabled(), settingsDhtWindowStart(), settingsDhtWindowEnd());
  sensorTimerSetEnabled(settingsStationRole() != STATION_ROLE_NONE);
  waterLevelSetVisionEnabled(false);
  settingsSetAiVisionEnabled(false);

  bool connected = wifiConnectBest(20000);
  if (connected) {
    timeKeeperBegin();
  } else {
    deviceLog("WiFi not connected -- loop() will retry every 5s (needs 2.4GHz AP)");
  }

  if (MDNS.begin(stationHostname)) {
    MDNS.addService("http", "tcp", 80);
    deviceLog("mDNS ready: http://%s.local", stationHostname);
  } else {
    deviceLog("mDNS setup failed");
  }

  ArduinoOTA.setHostname(stationHostname);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    cloudDeviceSetOtaActive(true);
    captureTimerSetEnabled(false);
    sensorTimerSetEnabled(false);
    waterLevelSetVisionEnabled(false);
    waterLevelCancelAll();
    deviceLog("OTA update starting...");
  });
  ArduinoOTA.onEnd([]() {
    cloudDeviceSetOtaActive(false);
    deviceLog("OTA update complete, rebooting");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static int lastLoggedPct = -1;
    int pct = total ? (int)((progress * 100UL) / total) : 0;
    if (pct / 20 != lastLoggedPct / 20) {
      lastLoggedPct = pct;
      deviceLog("OTA progress: %d%%", pct);
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    cloudDeviceSetOtaActive(false);
    deviceLog("OTA error [%u] -- continuing", (unsigned)error);
    captureTimerSetEnabled(settingsTimerEnabled());
    sensorTimerSetEnabled(settingsStationRole() != STATION_ROLE_NONE);
    waterLevelSetVisionEnabled(false);
  });
  ArduinoOTA.begin();
  stationDiscoveryBegin();

  startCameraServer();

  if (connected) {
    deviceLog("Ready: http://%s  http://%s.local", WiFi.localIP().toString().c_str(), stationHostname);
  } else {
    deviceLog("HTTP server up, waiting for WiFi");
  }
}

#define NTP_RESYNC_INTERVAL_MS (6UL * 60 * 60 * 1000)
#define WIFI_CHECK_INTERVAL_MS 5000

void loop() {
  static bool wasConnected = false;
  static bool ntpStarted = false;
  static uint32_t lastNtpResyncMs = 0;
  static uint32_t lastWifiCheckMs = 0;
  static uint32_t lastWifiRetryMs = 0;

  ArduinoOTA.handle();
  stationDiscoveryLoop();
  cloudDeviceLoop();

  uint32_t nowMs = millis();
  if (nowMs - lastWifiCheckMs >= WIFI_CHECK_INTERVAL_MS) {
    lastWifiCheckMs = nowMs;

    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !wasConnected) {
      logWifiConnected("(re)connected");
      if (!ntpStarted) {
        timeKeeperBegin();
        ntpStarted = true;
        lastNtpResyncMs = nowMs;
      }
    } else if (!connected && wasConnected) {
      deviceLog("WiFi lost (%s), reconnecting...", wifiStatusName(WiFi.status()));
      wifiConnectBest(8000);
    } else if (!connected && !wasConnected) {
      deviceLog("WiFi still down (%s)", wifiStatusName(WiFi.status()));
      if (nowMs - lastWifiRetryMs >= 20000) {
        lastWifiRetryMs = nowMs;
        wifiConnectBest(8000);
      }
    }
    wasConnected = (WiFi.status() == WL_CONNECTED);

    if (wasConnected && ntpStarted && nowMs - lastNtpResyncMs > NTP_RESYNC_INTERVAL_MS) {
      deviceLog("Periodic NTP re-sync");
      timeKeeperBegin();
      lastNtpResyncMs = nowMs;
    }
  }

  delay(10);
}
