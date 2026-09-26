#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_init.h"
#include "display.h"
#include "ota_app.h"
#include "sensores_ambiente.h"
#include "tempo.h"
#include "wifi_app.h"

void app_init_default_shared(app_shared_data_t *shared)
{
    if (shared == NULL) {
        return;
    }

    shared->wifi_event_group = NULL;
    shared->ultimo_envio_res = ESP_FAIL;
    shared->ultimo_http_status = -1;
    shared->total_envios_ok = 0;
    shared->total_envios_erro = 0;
    shared->boot_sync_source = "NONE";
}

esp_err_t app_init_inicializar_sistema(app_handles_t *handles, app_shared_data_t *shared)
{
    if (handles == NULL || shared == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    handles->i2c_bus = NULL;
    handles->ds3231_dev = NULL;
    handles->oled_handle = NULL;

    setenv("TZ", "UTC0", 1);
    tzset();

    ESP_ERROR_CHECK(ota_app_inicializar());
    ESP_ERROR_CHECK(wifi_app_inicializar_sta(shared));
    /* ADC/GDMA is initialized by its pinned owner task, not by app_main. */
    ESP_ERROR_CHECK(tempo_inicializar_i2c_rtc(handles));
    /* Network time synchronization runs after the acquisition task starts. */

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_ERROR_CHECK(display_inicializar(handles));
    ESP_ERROR_CHECK(sensores_ambiente_inicializar());

    return ESP_OK;
}
