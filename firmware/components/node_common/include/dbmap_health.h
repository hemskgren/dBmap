#pragma once

#include "cJSON.h"
#include "dbmap_types.h"
#include "esp_err.h"

/**
 * Optional enricher for hello/health/keepalive JSON (for example Ear capabilities).
 * Output leaves this unset. Called after the common state fields are filled.
 */
typedef void (*dbmap_health_enrich_fn)(cJSON *state, const dbmap_effective_t *cfg, bool include_diag);

/**
 * Replace all enrichers with a single callback (legacy helper).
 * Prefer dbmap_health_add_enricher when multiple components contribute fields.
 */
void dbmap_health_set_enricher(dbmap_health_enrich_fn fn);

/** Append an enricher (no-op if NULL or the table is full). */
void dbmap_health_add_enricher(dbmap_health_enrich_fn fn);

esp_err_t dbmap_publish_hello(const dbmap_effective_t *cfg);
esp_err_t dbmap_publish_health(const dbmap_effective_t *cfg);
esp_err_t dbmap_publish_status(const dbmap_effective_t *cfg, const char *status_json);
