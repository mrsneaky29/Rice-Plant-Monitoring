#include "station_discovery.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <time.h>
#include "settings_store.h"
#include "station_identity.h"
#include "sd_storage.h"
#include "time_keeper.h"
#include "device_log.h"

static const uint16_t DISCOVERY_PORT = 4210;
static WiFiUDP udp;
static int lastSentSlot = -1;
static int heardAbsoluteSlot = -1;

static void sendAnnouncement(const char *kind) {
  if (WiFi.status() != WL_CONNECTED) return;
  char id[16], name[32], packet[320];
  stationIdentityId(id, sizeof(id));
  settingsStationName(name, sizeof(name));
  snprintf(packet, sizeof(packet),
           "{\"type\":\"%s\",\"station_id\":\"%s\",\"station_no\":%d,\"name\":\"%s\",\"role\":\"%s\",\"ip\":\"%s\",\"mac\":\"%s\",\"photo_count\":%u}",
           kind, id, settingsStationNumber(), name, settingsStationRoleName(settingsStationRole()),
           WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str(), (unsigned)sdStoragePhotoCount());
  udp.beginPacket(IPAddress(255, 255, 255, 255), DISCOVERY_PORT);
  udp.write((const uint8_t *)packet, strlen(packet));
  udp.endPacket();
}

void stationDiscoveryBegin() {
  udp.begin(DISCOVERY_PORT);
  deviceLog("Station discovery UDP port %u", (unsigned)DISCOVERY_PORT);
}

void stationDiscoveryLoop() {
  if (WiFi.status() != WL_CONNECTED) return;

  int size = udp.parsePacket();
  if (size > 0) {
    char buf[384];
    int n = udp.read(buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = 0;
      if (strstr(buf, "PC_DISCOVER") || strstr(buf, "PC_ONLINE")) sendAnnouncement("STATION_ANNOUNCE");
      if (strstr(buf, "STATION_PING")) heardAbsoluteSlot = (int)(time(NULL) / 1200);
    }
  }

  int minute = timeKeeperLocalMinuteOfDay();
  if (minute < 0) return;
  int slotInHour = (minute % 60) / 20;
  int absoluteSlot = (int)(time(NULL) / 1200);
  if (absoluteSlot == lastSentSlot) return;

  int leader = slotInHour + 1;
  int station = settingsStationNumber();
  int rank = (station - leader + 3) % 3;
  int secondsIntoSlot = (int)(time(NULL) % 1200);
  if (rank == 0 || (heardAbsoluteSlot != absoluteSlot && secondsIntoSlot >= rank * 30)) {
    sendAnnouncement("STATION_PING");
    lastSentSlot = absoluteSlot;
    heardAbsoluteSlot = absoluteSlot;
  }
}
