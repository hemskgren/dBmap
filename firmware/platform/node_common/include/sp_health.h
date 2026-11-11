#pragma once

#include "cJSON.h"
#include "sp_types.h"
#include "esp_err.h"

/**
 * Optional enricher for hello/health/keepalive JSON (for example Ear capabilities).
 * Output leaves this unset. Called after the common state fields are filled.
 */
typedef void (*sp_health_enrich_fn)(cJSON *state, const sp_effective_t *cfg, bool include_diag);

/**
 * Replace all enrichers with a single callback (legacy helper).
 * Prefer sp_health_add_enricher when multiple components contribute fields.
 */
void sp_health_set_enricher(sp_health_enrich_fn fn);

/** Append an enricher (no-op if NULL or the table is full). */
void sp_health_add_enricher(sp_health_enrich_fn fn);

esp_err_t sp_publish_hello(const sp_effective_t *cfg);
esp_err_t sp_publish_health(const sp_effective_t *cfg);
esp_err_t sp_publish_status(const sp_effective_t *cfg, const char *status_json);
