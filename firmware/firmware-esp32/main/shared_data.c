#include "shared_data.h"
#include <math.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_timer.h"

/* Only the persistence task owns this handle after startup. Flash operations
 * never run with xSharedDataMutex held or from the electrical task. */
static nvs_handle_t energia_nvs;
static double energia_gravada;
static int64_t ultima_gravacao_us;
static int64_t proxima_tentativa_us;

SemaphoreHandle_t xSharedDataMutex;
static medicoes_eletricas_t ultima_medicao = {.resultado = ESP_ERR_INVALID_STATE};

/* Call once during startup, before creating any consumers/producers. */
esp_err_t shared_data_init(void)
{
    if (xSharedDataMutex != NULL) {
        return ESP_OK;
    }
    esp_err_t res = nvs_flash_init();
    if (res != ESP_OK) { return res; }
    res = nvs_open("energia", NVS_READWRITE, &energia_nvs);
    if (res != ESP_OK) { return res; }
    double recuperada = 0.0;
    size_t tamanho = sizeof(recuperada);
    res = nvs_get_blob(energia_nvs, "energia_kwh", &recuperada, &tamanho);
    if (res != ESP_OK && res != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(energia_nvs);
        return res; /* Do not overwrite a record we could not read. */
    }
    if (res == ESP_OK && tamanho == sizeof(recuperada) &&
        isfinite(recuperada) && recuperada > 0.0) {
        ultima_medicao.energia_kwh = recuperada;
    }
    energia_gravada = ultima_medicao.energia_kwh;
    ultima_gravacao_us = esp_timer_get_time();
    xSharedDataMutex = xSemaphoreCreateMutex();
    if (xSharedDataMutex == NULL) { nvs_close(energia_nvs); }
    return xSharedDataMutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t shared_data_persistir_energia(void)
{
    medicoes_eletricas_t snapshot;
    esp_err_t res = shared_data_get_medicoes(&snapshot);
    if (res != ESP_OK) { return res; }
    int64_t agora = esp_timer_get_time();
    double energia = snapshot.energia_kwh;
    if (!isfinite(energia) || energia <= energia_gravada ||
        agora < proxima_tentativa_us ||
        (agora - ultima_gravacao_us < 300000000LL && energia - energia_gravada < 0.005)) {
        return ESP_OK;
    }
    /* Rate-limit retries as well, including after a failed commit. */
    proxima_tentativa_us = agora + 10000000LL;
    res = nvs_set_blob(energia_nvs, "energia_kwh", &energia, sizeof(energia));
    if (res == ESP_OK) { res = nvs_commit(energia_nvs); }
    if (res == ESP_OK) {
        energia_gravada = energia;
        ultima_gravacao_us = agora;
        proxima_tentativa_us = 0;
    }
    return res;
}

esp_err_t shared_data_set_medicoes(const medicoes_eletricas_t *novas_medicoes)
{
    if (novas_medicoes == NULL) { return ESP_ERR_INVALID_ARG; }
    if (xSharedDataMutex == NULL) { return ESP_ERR_INVALID_STATE; }
    if (xSemaphoreTake(xSharedDataMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    ultima_medicao = *novas_medicoes;
    xSemaphoreGive(xSharedDataMutex);
    return ESP_OK;
}

esp_err_t shared_data_get_medicoes(medicoes_eletricas_t *dest)
{
    if (dest == NULL) { return ESP_ERR_INVALID_ARG; }
    if (xSharedDataMutex == NULL) { return ESP_ERR_INVALID_STATE; }
    if (xSemaphoreTake(xSharedDataMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *dest = ultima_medicao;
    xSemaphoreGive(xSharedDataMutex);
    return ESP_OK;
}
