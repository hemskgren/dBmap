#pragma once

#include "dbmap_types.h"

/* Boot check, then every 60 minutes. On mismatch with id/sha/size, attempts one OTA download. */
void dbmap_desired_poll_start(dbmap_effective_t *cfg);
