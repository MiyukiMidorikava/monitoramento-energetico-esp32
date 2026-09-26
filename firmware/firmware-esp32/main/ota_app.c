#include "ota_app.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "config_app.h"
#include "shared_data.h"
#include "wifi_app.h"
#include "ota_signing_key.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "freertos/task.h"

static const char *TAG = "ota_app";
static atomic_flag atualizando = ATOMIC_FLAG_INIT;
static atomic_bool validacao_pendente;
static atomic_uint envios_consecutivos;
static int64_t inicio_validacao_us;
/* Published once by the sender before the atomic count reaches ten. */
static int64_t conclusao_validacao_us;

#define OTA_VALIDACAO_TIMEOUT_US 120000000LL
#define OTA_VALIDACAO_ENVIOS 10U

esp_err_t ota_app_inicializar(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) { return ESP_ERR_NOT_FOUND; }
    esp_ota_img_states_t estado;
    esp_err_t res = esp_ota_get_state_partition(running, &estado);
    /* Factory images have no OTA state. */
    if (res == ESP_ERR_NOT_FOUND) { return ESP_OK; }
    if (res != ESP_OK) { return res; }
    atomic_store(&envios_consecutivos, 0);
    inicio_validacao_us = esp_timer_get_time();
    atomic_store(&validacao_pendente, estado == ESP_OTA_IMG_PENDING_VERIFY);
    if (estado == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "OTA pendente: aguardando 10 HTTP 200 consecutivos (prazo 120 s)");
    }
    return ESP_OK;
}

/* Only the telemetry task publishes results. No flash access on this path. */
void ota_app_registrar_telemetria(esp_err_t resultado, int status)
{
    if (!atomic_load(&validacao_pendente)) { return; }
    /* Latch completion: the owner may run after the next HTTP attempt. */
    if (atomic_load(&envios_consecutivos) >= OTA_VALIDACAO_ENVIOS) { return; }
    if (resultado == ESP_OK && status == 200) {
        unsigned contagem = atomic_load(&envios_consecutivos) + 1;
        if (contagem == OTA_VALIDACAO_ENVIOS) {
            conclusao_validacao_us = esp_timer_get_time();
        }
        atomic_store(&envios_consecutivos, contagem);
    } else {
        atomic_store(&envios_consecutivos, 0);
    }
}

/* Sole owner of validation/rollback flash operations: ota_task on Core 0. */
static void ota_validar_boot(void)
{
    if (!atomic_load(&validacao_pendente)) { return; }
    if (atomic_load(&envios_consecutivos) >= OTA_VALIDACAO_ENVIOS &&
        conclusao_validacao_us - inicio_validacao_us < OTA_VALIDACAO_TIMEOUT_US) {
        esp_err_t res = esp_ota_mark_app_valid_cancel_rollback();
        if (res == ESP_OK) {
            atomic_store(&validacao_pendente, false);
            ESP_LOGI(TAG, "OTA confirmada apos 10 HTTP 200 consecutivos");
            return;
        }
        ESP_LOGW(TAG, "Falha ao confirmar OTA: %s", esp_err_to_name(res));
    }
    if (esp_timer_get_time() - inicio_validacao_us >= OTA_VALIDACAO_TIMEOUT_US) {
        ESP_LOGE(TAG, "OTA sem validacao no prazo; revertendo imagem");
        esp_err_t res = esp_ota_mark_app_invalid_rollback_and_reboot();
        /* If no valid fallback exists, preserve the pending state and retry on
         * next boot; do not silently accept the unverified application. */
        ESP_LOGE(TAG, "Rollback retornou %s; reiniciando", esp_err_to_name(res));
        esp_restart();
        return;
    }
}

/* Companion "<OTA_URL>.sig" published alongside firmware.bin: a raw ECDSA
 * P-256 DER signature (see tools/sign_firmware.py) over the SHA-256 of the
 * whole image. Detached rather than prepended so an unmodified Caddy
 * static file server can keep serving both files as-is. */
static esp_err_t ota_baixar_assinatura(uint8_t *buffer, size_t capacidade, size_t *tamanho_out)
{
    char url[256];
    int n = snprintf(url, sizeof(url), "%s.sig", CONFIG_MONITOR_OTA_URL);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 5000,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t res = esp_http_client_set_header(client, "X-API-Key", API_TOKEN);
    if (res != ESP_OK) {
        esp_http_client_cleanup(client);
        return res;
    }
    res = esp_http_client_open(client, 0);
    if (res != ESP_OK) {
        esp_http_client_cleanup(client);
        return res;
    }
    int64_t length = esp_http_client_fetch_headers(client);
    if (length <= 0 || length > (int64_t)capacidade ||
        esp_http_client_get_status_code(client) != 200) {
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t total = 0;
    while (total < (size_t)length) {
        int lidos = esp_http_client_read(client, (char *)buffer + total, (size_t)length - total);
        if (lidos <= 0) {
            esp_http_client_cleanup(client);
            return ESP_ERR_INVALID_RESPONSE;
        }
        total += (size_t)lidos;
    }
    esp_http_client_cleanup(client);
    *tamanho_out = total;
    return ESP_OK;
}

/* Authenticity gate: SHA256-diff detection alone only proves the image
 * differs from what's running, not who produced it. Verified with the
 * public key in ota_signing_key.h; the matching private key never leaves
 * whichever machine signs releases (see tools/sign_firmware.py). */
static esp_err_t ota_verificar_assinatura(const uint8_t *hash, const uint8_t *assinatura, size_t assinatura_len)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int ret = mbedtls_pk_parse_public_key(&pk,
                                          (const unsigned char *)OTA_SIGNING_PUBLIC_KEY_PEM,
                                          strlen(OTA_SIGNING_PUBLIC_KEY_PEM) + 1);
    if (ret != 0) {
        mbedtls_pk_free(&pk);
        return ESP_FAIL;
    }
    ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, assinatura, assinatura_len);
    mbedtls_pk_free(&pk);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ota_verificar_e_atualizar(void)
{
    if (atomic_load(&validacao_pendente)) { return ESP_ERR_INVALID_STATE; }
    if (atomic_flag_test_and_set(&atualizando)) { return ESP_ERR_INVALID_STATE; }
    esp_err_t res = ESP_FAIL;
    esp_ota_handle_t handle = 0;
    bool iniciado = false;
    size_t total = 0;
    uint8_t buffer[1024];
    uint8_t hash[32];
    uint8_t assinatura[128];
    size_t assinatura_len = 0;
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_http_client_handle_t client = NULL;
    if (part == NULL) { res = ESP_ERR_NOT_FOUND; goto cleanup; }

    esp_http_client_config_t config = {
        .url = CONFIG_MONITOR_OTA_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 5000,
        .keep_alive_enable = true,
        .disable_auto_redirect = true,
    };
    client = esp_http_client_init(&config);
    if (client == NULL) { res = ESP_ERR_NO_MEM; goto cleanup; }
    res = esp_http_client_set_header(client, "X-API-Key", API_TOKEN);
    if (res != ESP_OK) { goto cleanup; }
    res = esp_http_client_open(client, 0);
    if (res != ESP_OK) { goto cleanup; }
    int64_t length = esp_http_client_fetch_headers(client);
    if (length < 0 || esp_http_client_get_status_code(client) != 200) {
        res = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    if (length > part->size) { res = ESP_ERR_INVALID_SIZE; goto cleanup; }

    /* Assemble the descriptor even when HTTP splits it across short reads. */
    const size_t prefix = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    const size_t header_size = prefix + sizeof(esp_app_desc_t);
    _Static_assert(sizeof(buffer) >= sizeof(esp_image_header_t) +
                   sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t),
                   "OTA buffer must fit image descriptor");
    while (total < header_size) {
        int n = esp_http_client_read(client, (char *)buffer + total, header_size - total);
        if (n <= 0) { res = ESP_ERR_INVALID_RESPONSE; goto cleanup; }
        total += n;
    }
    esp_app_desc_t candidate;
    memcpy(&candidate, buffer + prefix, sizeof(candidate));
    if (buffer[0] != ESP_IMAGE_HEADER_MAGIC || candidate.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        res = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    const esp_app_desc_t *running = esp_app_get_description();
    if (memcmp(candidate.app_elf_sha256, running->app_elf_sha256,
               sizeof(candidate.app_elf_sha256)) == 0) {
        res = ESP_ERR_NOT_FINISHED;
        goto cleanup;
    }

    /* Incremental erase avoids erasing the entire bank before downloading. */
    res = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (res != ESP_OK) { goto cleanup; }
    iniciado = true;
    res = esp_ota_write(handle, buffer, total);
    if (res != ESP_OK) { goto cleanup; }
    mbedtls_sha256_update(&sha_ctx, buffer, total);
    while (true) {
        int n = esp_http_client_read(client, (char *)buffer, sizeof(buffer));
        if (n < 0) { res = ESP_FAIL; goto cleanup; }
        if (n == 0) { break; }
        if ((size_t)n > part->size - total) { res = ESP_ERR_INVALID_SIZE; goto cleanup; }
        res = esp_ota_write(handle, buffer, n);
        if (res != ESP_OK) { goto cleanup; }
        mbedtls_sha256_update(&sha_ctx, buffer, (size_t)n);
        total += n;
        vTaskDelay(1);
    }
    if (!esp_http_client_is_complete_data_received(client) ||
        (length > 0 && total != (size_t)length)) {
        res = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    res = esp_ota_end(handle);
    iniciado = false; /* esp_ota_end frees the handle even on validation error. */
    if (res != ESP_OK) { goto cleanup; }
    mbedtls_sha256_finish(&sha_ctx, hash);

    /* SHA256-diff (above, via app_elf_sha256) only proves this image is
     * different from what's running -- it says nothing about who produced
     * it. Reject before ever selecting this bank as next boot. */
    res = ota_baixar_assinatura(assinatura, sizeof(assinatura), &assinatura_len);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao baixar assinatura OTA: %s", esp_err_to_name(res));
        goto cleanup;
    }
    if (ota_verificar_assinatura(hash, assinatura, assinatura_len) != ESP_OK) {
        ESP_LOGE(TAG, "Assinatura OTA invalida; imagem rejeitada, banco de boot inalterado");
        res = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    res = esp_ota_set_boot_partition(part);
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "OTA validado e assinatura confirmada: %u bytes, proximo boot em %s",
                 (unsigned)total, part->label);
    }
cleanup:
    mbedtls_sha256_free(&sha_ctx);
    if (iniciado) { esp_ota_abort(handle); }
    if (client != NULL) { esp_http_client_cleanup(client); }
    atomic_flag_clear(&atualizando);
    return res;
}

/* Deliberately not subscribed to the task watchdog: this task sleeps up to
 * OTA_TASK_INTERVAL_MS (30 min) by design between checks, and a download can
 * run for several seconds -- both far past the global 5 s watchdog timeout.
 * Its own safety net is the rollback-confirmation window, not the TWDT. */
void ota_task(void *arg)
{
    app_runtime_t *runtime = arg;
    while (atomic_load(&validacao_pendente)) {
        ota_validar_boot();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    while (true) {
        /* This task is created at boot, before Wi-Fi has associated, so the
         * very first iteration usually finds wifi_app_esta_conectado() false.
         * Retrying on a short interval here (instead of always falling
         * through to the 30 min floor below) means a device that only stays
         * up briefly still gets a real OTA check, and a longer Wi-Fi outage
         * doesn't silently cost up to 30 extra minutes before the next try. */
        uint32_t espera_ms = OTA_TASK_INTERVAL_MS;
        if (wifi_app_esta_conectado(&runtime->shared)) {
            esp_err_t res = ota_verificar_e_atualizar();
            if (res == ESP_OK) {
                ESP_LOGI(TAG, "Reiniciando para aplicar OTA");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            } else if (res != ESP_ERR_NOT_FINISHED) {
                ESP_LOGW(TAG, "OTA falhou: %s; boot atual preservado", esp_err_to_name(res));
            }
        } else {
            /* No check was actually attempted; the 30 min floor only applies
             * after a real attempt (success, failure, or same-image skip). */
            espera_ms = OTA_TASK_WIFI_RETRY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(espera_ms));
    }
}
