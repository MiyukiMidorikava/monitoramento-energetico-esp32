#pragma once

#include "esp_err.h"

#include "shared_data.h"

void app_init_default_shared(app_shared_data_t *shared);
esp_err_t app_init_inicializar_sistema(app_handles_t *handles, app_shared_data_t *shared);