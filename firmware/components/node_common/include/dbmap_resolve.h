#pragma once

#include <stddef.h>

#include "esp_err.h"

/* Resolve hostname to an IPv4 dotted string. For *.local names, query mDNS with
 * retries; other names use getaddrinfo. ip_out receives a NUL-terminated address. */
esp_err_t dbmap_resolve_ipv4(const char *hostname, char *ip_out, size_t ip_out_len);
