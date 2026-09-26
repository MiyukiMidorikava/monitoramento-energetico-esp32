#pragma once

#include <time.h>

#include "esp_err.h"

#include "shared_data.h"

esp_err_t tempo_inicializar_i2c_rtc(app_handles_t *handles);
esp_err_t tempo_sincronizar_boot(app_handles_t *handles, app_shared_data_t *shared);
/* Periodic resync so a boot-time NTP failure (or ordinary RTC drift) doesn't
 * stick for the entire uptime; does not touch shared->boot_sync_source. */
esp_err_t tempo_resincronizar_ntp(app_handles_t *handles);
esp_err_t tempo_ler_sistema(struct tm *tempo);
esp_err_t tempo_ler_ds3231(const app_handles_t *handles, struct tm *tempo);
