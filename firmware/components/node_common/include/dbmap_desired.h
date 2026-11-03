#pragma once

#include "dbmap_types.h"

/**
 * Synchronous Desired-firmware check for boot sequencing.
 * On mismatch with usable id/sha/size, attempts one OTA download (may reboot).
 * Call after Wi-Fi + MQTT are up and before starting Acoustic Monitor.
 */
void dbmap_desired_boot_check(dbmap_effective_t *cfg);

/**
 * Start periodic Desired poll (boot check then every 60 minutes).
 * Does not stop or restart Acoustic Monitor. Ear should call
 * dbmap_desired_boot_check() before starting audio so OTA can finish first;
 * poll_start may then repeat a check (harmless if firmware already matches).
 */
void dbmap_desired_poll_start(dbmap_effective_t *cfg);
