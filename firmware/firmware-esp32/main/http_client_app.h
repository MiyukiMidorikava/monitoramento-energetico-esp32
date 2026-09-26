#pragma once

#include "esp_err.h"

esp_err_t http_client_app_enviar_telemetria(const char *payload_json, int *status_code_out);
