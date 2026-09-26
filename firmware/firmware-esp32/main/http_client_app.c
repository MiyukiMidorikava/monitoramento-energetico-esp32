#include <string.h>
#include <limits.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "config_app.h"
#include "http_client_app.h"

static const char *TAG = "http_client_app";

/* Sole owner: task_envio_telemetria. OTA uses its own client. */
static esp_http_client_handle_t telemetry_client;

/* Monotonic deadline: repeated calls cannot bypass the server cooldown. */
static int64_t retry_after_us;

esp_err_t http_client_app_enviar_telemetria(const char *payload_json, int *status_code_out)
{
    if (status_code_out != NULL) *status_code_out = -1;
    if (payload_json == NULL) return ESP_ERR_INVALID_ARG;
    if (esp_timer_get_time() < retry_after_us) return ESP_ERR_INVALID_STATE;
    size_t length = strlen(payload_json);
    if (length > INT_MAX) return ESP_ERR_INVALID_SIZE;


    esp_err_t err;
    if (telemetry_client == NULL) {
        esp_http_client_config_t config = {
            .url = FLASK_ENDPOINT_URL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 5000,
            /* TCP keep-alive probes. HTTP/TLS reuse comes from retaining this
             * handle and consuming each response; Caddy may still close it. */
            .keep_alive_enable = true,
            .disable_auto_redirect = true,
        };
        telemetry_client = esp_http_client_init(&config);
        if (telemetry_client == NULL) return ESP_ERR_NO_MEM;
        err = esp_http_client_set_header(telemetry_client, "Content-Type", "application/json");
        if (err != ESP_OK) goto reset_client;
        err = esp_http_client_set_header(telemetry_client, "X-API-Key", API_TOKEN);
        if (err != ESP_OK) goto reset_client;
    }

    err = esp_http_client_set_post_field(telemetry_client, payload_json, (int)length);
    if (err != ESP_OK) goto reset_client;
    err = esp_http_client_perform(telemetry_client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(telemetry_client);
        if (status_code_out != NULL) *status_code_out = status_code;
        if (status_code == 401 || status_code == 403 || status_code == 429) {
            retry_after_us = esp_timer_get_time() + 60000000LL;
        }
        if (status_code < 200 || status_code >= 300) {
            /* Untrusted response bodies can echo credentials: never log them. */
            ESP_LOGW(TAG, "Resposta do servidor: status %d", status_code);
        }
    }
    /* esp_http_client_perform() is synchronous: the payload is fully written
     * before it returns. The stored pointer must be replaced by the next
     * set_post_field() before another perform(); it is not used in between.
     * Do NOT "release" it with set_post_field(NULL, 0) --
     * ESP-IDF's implementation treats that as clearing the body and deletes
     * the Content-Type header as a side effect (esp_http_client.c). The next
     * send would then see no Content-Type set and silently default to
     * "application/x-www-form-urlencoded", which the backend rejects with
     * 400. This is why only the very first POST after boot ever succeeded. */
    if (err != ESP_OK && esp_http_client_close(telemetry_client) != ESP_OK) {
        goto reset_client;
    }
    return err;

reset_client:
    esp_http_client_cleanup(telemetry_client);
    telemetry_client = NULL;
    return err;
}
