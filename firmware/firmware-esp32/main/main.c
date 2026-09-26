#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "app_init.h"
#include "tasks_app.h"

void app_main(void)
{
    ESP_LOGI("main", "Sistema de Monitoramento Energetico iniciado");

    static app_runtime_t runtime = {0};

    app_init_default_shared(&runtime.shared);
    runtime.medicoes.ts_source = "NONE";
    runtime.medicoes.dht_res = ESP_FAIL;
    /* Placeholder until the first internal-sensor reading completes. */
    runtime.medicoes.temperatura = 25.0f;
    runtime.medicoes.temperatura_ambiente = 25.0f;
    runtime.medicoes.umidade = 50.0f;
    runtime.medicoes.rtc_res = ESP_FAIL;
    runtime.medicoes.zmpt_res = ESP_FAIL;
    runtime.medicoes.sct_res = ESP_FAIL;

    ESP_ERROR_CHECK(app_init_inicializar_sistema(&runtime.handles, &runtime.shared));

    runtime.mutex = xSemaphoreCreateMutex();
    if (runtime.mutex == NULL) {
        ESP_LOGE("main", "Falha ao criar mutex global de runtime");
        /* A pending OTA image must reboot on startup failure, not remain idle
         * forever before its validation task can start. */
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    ESP_ERROR_CHECK(shared_data_init());
    ESP_ERROR_CHECK(app_tasks_start(&runtime));

    vTaskDelete(NULL);
}
