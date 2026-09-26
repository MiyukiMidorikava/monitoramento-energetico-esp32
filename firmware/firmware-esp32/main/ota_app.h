#pragma once

#include "esp_err.h"

esp_err_t ota_app_inicializar(void);
/* Single producer: telemetry task; transport failure/non-200 resets the streak. */
void ota_app_registrar_telemetria(esp_err_t resultado, int status);
/* ESP_OK: validated update selected for next boot (does not restart here).
 * ESP_ERR_NOT_FINISHED: same ELF already running. Concurrent calls rejected. */
esp_err_t ota_verificar_e_atualizar(void);
void ota_task(void *arg);
