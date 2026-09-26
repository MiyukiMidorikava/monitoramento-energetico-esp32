#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <math.h>

#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"

#include "telemetria.h"

/* Single caller (telemetry task). Includes room for full-precision numbers
 * and optional diagnostics; PrintPreallocated fails closed on overflow. */
#define TELEMETRIA_BUFFER_TAM 1024
static char telemetria_buffer[TELEMETRIA_BUFFER_TAM];

char *telemetria_montar_json(const app_medicoes_t *medicoes, const app_shared_data_t *shared)
{
    if (medicoes == NULL || shared == NULL) {
        return NULL;
    }

    if (medicoes->zmpt_res != ESP_OK || medicoes->sct_res != ESP_OK ||
        !isfinite(medicoes->tensao_rms) || !isfinite(medicoes->corrente_rms) ||
        !isfinite(medicoes->corrente_pre_correcao_a) || medicoes->corrente_pre_correcao_a < 0 ||
        !isfinite(medicoes->potencia_w) || !isfinite(medicoes->fator_potencia) ||
        !isfinite(medicoes->energia_kwh) || !isfinite(medicoes->potencia_va) ||
        !isfinite(medicoes->potencia_reativa_var) || !isfinite(medicoes->temperatura) ||
        !isfinite(medicoes->temperatura_ambiente) || !isfinite(medicoes->umidade)) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    const uint64_t uptime_ms = (uint64_t)(esp_timer_get_time() / 1000);
    /* ELF identity from the running image; not the SHA-256 of the .bin file. */
    const esp_app_desc_t *app = esp_app_get_description();
    static const char hex[] = "0123456789abcdef";
    char firmware_sha256[65];
    for (size_t i = 0; i < sizeof app->app_elf_sha256; ++i) {
        firmware_sha256[2 * i] = hex[app->app_elf_sha256[i] >> 4];
        firmware_sha256[2 * i + 1] = hex[app->app_elf_sha256[i] & 15];
    }
    firmware_sha256[64] = '\0';
    char uptime_decimal[21];
    int n = snprintf(uptime_decimal, sizeof uptime_decimal, "%" PRIu64, uptime_ms);
    if (n < 0 || (size_t)n >= sizeof uptime_decimal ||
        cJSON_AddStringToObject(root, "device", "esp32-energia") == NULL ||
        cJSON_AddStringToObject(root, "firmware_sha256", firmware_sha256) == NULL ||
        cJSON_AddRawToObject(root, "uptime_ms", uptime_decimal) == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *status = cJSON_AddObjectToObject(root, "status");
    cJSON *dados = cJSON_AddObjectToObject(root, "dados");
    if (status == NULL || dados == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    /* uxTaskGetStackHighWaterMark(NULL) reports the caller's own task --
     * always task_envio_telemetria here, the sole caller of this function.
     * StackType_t is uint8_t on the ESP32/ESP32-S3 FreeRTOS port, so the
     * watermark is already in bytes; no word-to-byte conversion needed. */
    if (cJSON_AddNumberToObject(status, "heap_livre_bytes", (double)esp_get_free_heap_size()) == NULL ||
        cJSON_AddNumberToObject(status, "reset_reason", (int)esp_reset_reason()) == NULL ||
        cJSON_AddNumberToObject(status, "dht_valido", medicoes->dht_res == ESP_OK ? 1 : 0) == NULL ||
        cJSON_AddNumberToObject(status, "corrente_pre_correcao_a", medicoes->corrente_pre_correcao_a) == NULL ||
        cJSON_AddNumberToObject(status, "stack_min_bytes", (double)uxTaskGetStackHighWaterMark(NULL)) == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    wifi_ap_record_t ap_info = {0};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK &&
        cJSON_AddNumberToObject(status, "wifi_rssi", (int)ap_info.rssi) == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    if (cJSON_AddNumberToObject(dados, "corrente_rms_a", medicoes->corrente_rms) == NULL ||
        cJSON_AddNumberToObject(dados, "tensao_rms_v", medicoes->tensao_rms) == NULL ||
        cJSON_AddNumberToObject(dados, "potencia_ativa_w", medicoes->potencia_w) == NULL ||
        cJSON_AddNumberToObject(dados, "fator_potencia", medicoes->fator_potencia) == NULL ||
        cJSON_AddNumberToObject(dados, "energia_kwh", medicoes->energia_kwh) == NULL ||
        cJSON_AddNumberToObject(dados, "potencia_va", medicoes->potencia_va) == NULL ||
        cJSON_AddNumberToObject(dados, "potencia_reativa_var", medicoes->potencia_reativa_var) == NULL ||
        cJSON_AddNumberToObject(dados, "temperatura_c", medicoes->temperatura) == NULL ||
        cJSON_AddNumberToObject(dados, "temperatura_ambiente_c", medicoes->temperatura_ambiente) == NULL ||
        cJSON_AddNumberToObject(dados, "umidade_pct", medicoes->umidade) == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    if (!cJSON_PrintPreallocated(root, telemetria_buffer, TELEMETRIA_BUFFER_TAM, false)) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_Delete(root);
    return telemetria_buffer;
}
