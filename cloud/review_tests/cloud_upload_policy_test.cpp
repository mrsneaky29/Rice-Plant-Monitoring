#include "../firmware/ESP32_Station/cloud_upload_policy.h"

// An old SD original must stay outside the queue, including after a reset.
static_assert(!cloudPhotoShouldUpload(false, false, false), "skip existing original");
static_assert(cloudPhotoShouldUpload(false, true, false), "retry new capture after reboot");
static_assert(!cloudPhotoShouldUpload(true, true, false), "stop retrying acknowledged capture");
static_assert(!cloudPhotoShouldUpload(true, false, true), "acknowledgement wins over opt in");
static_assert(cloudPhotoShouldUpload(false, false, true), "explicit historical opt in");

static_assert(cloudUploadBackoffIsReady(0, false, 0), "initial upload is ready");
static_assert(cloudUploadBackoffIsReady(0x80000001U, false, 0), "no 24-day blackout");
static_assert(cloudUploadBackoffIsReady(0xffffffffU, false, 100), "inactive stale deadline ignored");
static_assert(!cloudUploadBackoffIsReady(999, true, 1000), "respect retry delay");
static_assert(cloudUploadBackoffIsReady(1000, true, 1000), "retry exactly at deadline");
static_assert(cloudUploadBackoffIsReady(1001, true, 1000), "late retry is ready");
static_assert(!cloudUploadBackoffIsReady(0xfffffff0U, true, 20), "delay crosses rollover");
static_assert(!cloudUploadBackoffIsReady(19, true, 20), "wrapped deadline still pending");
static_assert(cloudUploadBackoffIsReady(20, true, 20), "wrapped deadline reached");
