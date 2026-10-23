# README audit — 23 October 2026

This audit covers all four maintained READMEs in this repository: the root, original station firmware, cloud service, and cloud station firmware. It also checks the linked setup guide, engineering record and architecture image. Generated cache/build READMEs and private or synced reference material are outside the maintained documentation set.

The audit compares documented behavior with source. Live observations below are dated **22 October 2026**, not fresh hardware tests on 23 October. No camera commands, firmware flashes, deployment changes, imports or paid analysis requests were performed for this audit.

## Corrections

| Area | Correction and evidence |
| --- | --- |
| Internet access | The main operating path is the [password-protected public HTTPS dashboard](https://admin.riceplant.achantalabs.com). The original PC hub and on-camera HTTP controls remain a separate local-network interface. Cloud sessions, CSRF and station-bound authentication are implemented in [the cloud service](../cloud/server/app.py); they do not add authentication to the original HTTP interfaces. |
| Analysis status | Live cloud analysis was enabled for new ordinary uploads at the 22 October checkpoint; the old disabled claim and diagram label were stale. Fresh installations default to off in [the cloud service](../cloud/server/app.py). Enabled configuration is not proof of a completed fresh-photo report. |
| Sleep and rollout | The old sleep-off diagram label was stale. Plant 2 reported sleep on with a one-hour interval on 22 October, while capture reliability and a complete wake/capture/upload/sleep cycle remained unverified. Plants 1 and 3 had not completed cloud rollout. The original station variant has no deep-sleep lifecycle. |
| Capture feedback | The deployed 22 October website patch distinguishes capture acknowledgement from photo receipt, retains recent terminal command outcomes, and no longer treats a historical upload-error count as a current error. The initial documentation-only audit did not publish the implementation; the subsequent 23 October resumption publishes the feedback code and tests and verifies their live browser behavior. |
| SD protection | Original [SD initialization](../ESP32_Station/sd_storage.cpp) requests formatting after mount failure and relocates legacy flat photographs; original HTTP controls permit deletion/erase. The [cloud SD implementation](../cloud/firmware/ESP32_Station/sd_storage.cpp) disables automatic formatting and relocation, and its protected build blocks deletion. Retention claims are now scoped to the correct variant. |
| Sensor roles | The original [PC hub](../PC_Hub/Monitoring_Hub.py) runs with `CAMERA_ONLY_MODE=True` and requests role changes and reboots for cameras reporting soil/DHT22 roles. Firmware support for explicit sensor roles does not mean this hub preserves those roles. |
| On-device analysis | The original station turns vision analysis off at boot, but [local HTTP routes](../ESP32_Station/app_httpd.cpp) can enable or request it. Its retained provider implementation is separate from hub/cloud analysis; boot-disabled must not be described as impossible to enable. |
| Historical imports | Legacy-import tooling and explicitly marked archive uploads do not queue paid cloud analysis. The firmware's opt-in historical-upload path sends ordinary uploads; it must not be described as automatically exempt from analysis. See [cloud configuration](../cloud/README.md#optional-analysis). |
| Setup and defaults | Documentation separates local and cloud build paths, identifies saved versus fresh timer settings, supplies an output archive for the package command, and treats example DHCP addresses as temporary. |
| Monitoring and evidence | Plant 2 reset diagnosis was deferred and the scheduled observation task was deleted. README wording no longer implies an active monitor or that old service/capture checks prove current camera reliability. |

## Subsequent website verification — 23 October 2026

After the documentation-only audit, read-only production records and the browser confirmed Plant 2 photo `2026-10-22__16-42-33-997.jpg`, a matching stored checksum, one completed `gpt-6.1-sol` job and its linked cloud history report. The job completed about 41 seconds after upload on 22 October. The feedback implementation/tests are now published, and a static-only update corrected the analysis page's imported-only labels. No camera command, new paid request, retry, backfill or service restart was needed for this follow-up.

## Validation and remaining limits

All maintained README sections were reviewed for architecture, setup, defaults, credentials, scheduling, capture/upload durability, analysis, backup boundaries, source maps, test instructions, historical observations and limitations. Relative documentation links and Markdown fences were checked; the architecture SVG was checked as XML; canonical cloud READMEs were synchronized with their repository copies. Only documentation is changed in this publication.

This audit does not establish current device connectivity, SD integrity, power-cut recovery, reliable hourly captures, the camera reset cause, a complete physical sleep cycle, repeatable analysis accuracy, Plants 1/3 rollout, or deployed backup improvements. Those remain separate acceptance checks. Git/Drive source archives exclude photos, databases, private configuration and compiled firmware; they are not monitoring-data backups.
