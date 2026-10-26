#pragma once

#include <stddef.h>

#include "esp_err.h"

/* Build https://<resolved-hub><api_path> using the same CA trust and mDNS lookup as provisioning. */
esp_err_t dbmap_hub_format_https_url(const char *hub_http_url, const char *api_path, char *url, size_t url_len);
const char *dbmap_hub_root_ca(void);
