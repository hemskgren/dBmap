#pragma once

#include "dbmap_types.h"

/* Boot check, then every 10 minutes. Logs only; does not download or flash. */
void dbmap_desired_poll_start(dbmap_effective_t *cfg);
