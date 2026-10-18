#pragma once

// The private build helper overrides this header only in its staged sketch.
// Status-only images cannot upload photos or execute cloud commands.
#define RICE_CLOUD_STATUS_ONLY 0

// Existing SD originals are collected separately. Only new captures with an
// upload-intent sidecar enter the queue unless explicitly enabled at build time.
#define RICE_CLOUD_UPLOAD_EXISTING_PHOTOS 0
#define RICE_SD_PROTECT_PHOTOS 1
