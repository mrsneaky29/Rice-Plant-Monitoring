#include "station_identity.h"
#include <Arduino.h>
#include "settings_store.h"

void stationIdentityId(char *out, size_t outSize) {
  snprintf(out, outSize, "plant-%d", settingsStationNumber());
}

void stationIdentityHostname(char *out, size_t outSize) {
  snprintf(out, outSize, "ricecam-%d", settingsStationNumber());
}
