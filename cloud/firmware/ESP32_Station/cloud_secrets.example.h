#ifndef CLOUD_SECRETS_H
#define CLOUD_SECRETS_H

// Copy to cloud_secrets.h (ignored by Git) and fill in a per-station token.
// Cloud sync and sleep are disabled unless CLOUD_DEVICE_TOKEN is configured.
#define CLOUD_DEVICE_TOKEN ""

// PEM root CA used to validate admin.riceplant.achantalabs.com.
// Keep normal certificate validation enabled; never use setInsecure().
#define CLOUD_ROOT_CA ""

#endif
