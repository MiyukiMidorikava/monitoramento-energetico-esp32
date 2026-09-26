#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "shared_data.h"

esp_err_t wifi_app_inicializar_sta(app_shared_data_t *shared);
bool wifi_app_esta_conectado(const app_shared_data_t *shared);
