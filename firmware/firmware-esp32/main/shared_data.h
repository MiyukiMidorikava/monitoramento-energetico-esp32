#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "ssd1306.h"

typedef struct {
    EventGroupHandle_t wifi_event_group;

    esp_err_t ultimo_envio_res;
    int ultimo_http_status;
    uint32_t total_envios_ok;
    uint32_t total_envios_erro;

    const char *boot_sync_source;
} app_shared_data_t;

typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_dev_handle_t ds3231_dev;
    ssd1306_handle_t oled_handle;
} app_handles_t;

typedef struct {
    float temperatura;
    float temperatura_ambiente;
    float umidade;
    float tensao_rms;
    float corrente_rms;
    float corrente_pre_correcao_a;
    float potencia_w;
    float potencia_va;
    float potencia_reativa_var;
    float fator_potencia;
    double energia_kwh;

    struct tm rtc_tempo;
    const char *ts_source;

    esp_err_t dht_res;
    esp_err_t rtc_res;
    esp_err_t zmpt_res;
    esp_err_t sct_res;
} app_medicoes_t;

/* Value-only snapshot: no DMA buffer pointers and no ambient data. */
typedef struct {
    float tensao_rms;
    float corrente_rms;
    float corrente_pre_correcao_a;
    float potencia_w;
    float potencia_va;
    float potencia_reativa_var;
    float fator_potencia;
    double energia_kwh;
    esp_err_t resultado;
    int64_t t_leitura_us;
} app_eletrica_snapshot_t;

typedef app_eletrica_snapshot_t medicoes_eletricas_t;
extern SemaphoreHandle_t xSharedDataMutex;
/* Initialize once before task creation. Set/get return timeout after 50 ms;
 * on failure the stored value and caller's destination remain unchanged. */
esp_err_t shared_data_init(void);
/* Single owner: call only from the Core 0 persistence task after init.
 * Copies under the measurement mutex, releases it before any NVS operation. */
esp_err_t shared_data_persistir_energia(void);
esp_err_t shared_data_set_medicoes(const medicoes_eletricas_t *novas_medicoes);
esp_err_t shared_data_get_medicoes(medicoes_eletricas_t *dest);

typedef struct {
    SemaphoreHandle_t mutex;
    bool tasks_started;

    app_handles_t handles;
    app_shared_data_t shared;
    app_medicoes_t medicoes;

} app_runtime_t;
