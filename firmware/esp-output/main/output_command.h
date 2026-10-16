#pragma once

#include "esp_err.h"

esp_err_t output_command_init(void);
void output_handle_command(const char *payload, int len, void *ctx);
