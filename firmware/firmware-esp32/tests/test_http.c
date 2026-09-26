#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_MONITOR_API_TOKEN "test-api-token"
#include "../main/http_client_app.c"

static int64_t now_us;
int64_t esp_timer_get_time(void) { return now_us; }

static int initialized, closed, cleaned, requests_sent;
static int response_status = 200;
static esp_err_t transport_result = ESP_OK, close_result = ESP_OK;
static const char *post_body;
static bool json_header, token_header;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    assert(strcmp(cfg->url, "https://energia.example.invalid/telemetria") == 0);
    assert(cfg->disable_auto_redirect);
    assert(cfg->crt_bundle_attach == esp_crt_bundle_attach);
    assert(cfg->keep_alive_enable && cfg->method == HTTP_METHOD_POST);
    ++initialized;
    json_header = token_header = false;
    return (void *)1;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t h, const char *key, const char *value)
{
    assert(h == (void *)1);
    if (strcmp(key, "Content-Type") == 0) json_header = strcmp(value, "application/json") == 0;
    if (strcmp(key, "X-API-Key") == 0) token_header = strcmp(value, "test-api-token") == 0;
    return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t h, const char *body, int length)
{
    assert(h == (void *)1 && body != NULL && length == (int)strlen(body));
    post_body = body;
    return ESP_OK;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t h)
{
    assert(h == (void *)1 && json_header && token_header && post_body != NULL);
    ++requests_sent;
    return transport_result;
}
int esp_http_client_get_status_code(esp_http_client_handle_t h) { (void)h; return response_status; }
esp_err_t esp_http_client_close(esp_http_client_handle_t h) { (void)h; ++closed; return close_result; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h) { (void)h; ++cleaned; return ESP_OK; }

int main(void)
{
    int status = 99;
    assert(http_client_app_enviar_telemetria(NULL, &status) == ESP_ERR_INVALID_ARG && status == -1);
    for (int i = 0; i < 4; ++i) {
        char body[32];
        snprintf(body, sizeof body, "{\"sequence\":%d}", i);
        assert(http_client_app_enviar_telemetria(body, &status) == ESP_OK && status == 200);
        assert(strcmp(post_body, body) == 0);
    }
    assert(initialized == 1 && closed == 0 && cleaned == 0 && requests_sent == 4);
    response_status = 429;
    assert(http_client_app_enviar_telemetria("{}", &status) == ESP_OK && status == 429);
    assert(initialized == 1 && closed == 0);
    const int denied[] = {401, 403, 429};
    now_us += 60000000;
    for (unsigned i = 0; i < sizeof denied / sizeof denied[0]; ++i) {
        response_status = denied[i];
        assert(http_client_app_enviar_telemetria("{}", &status) == ESP_OK);
        int before = requests_sent;
        response_status = 200;
        assert(http_client_app_enviar_telemetria("{}", &status) != ESP_OK);
        now_us += 59999999;
        assert(http_client_app_enviar_telemetria("{}", &status) != ESP_OK);
        assert(requests_sent == before);
        ++now_us;
        assert(http_client_app_enviar_telemetria("{}", &status) == ESP_OK);
        assert(requests_sent == before + 1);
    }
    transport_result = ESP_FAIL;
    assert(http_client_app_enviar_telemetria("{}", &status) == ESP_FAIL && status == -1);
    assert(closed == 1 && cleaned == 0);
    transport_result = ESP_OK;
    assert(http_client_app_enviar_telemetria("{}", &status) == ESP_OK);
    assert(initialized == 1);
    transport_result = close_result = ESP_FAIL;
    assert(http_client_app_enviar_telemetria("{}", &status) == ESP_FAIL);
    assert(cleaned == 1);
    transport_result = close_result = ESP_OK;
    assert(http_client_app_enviar_telemetria("{}", NULL) == ESP_OK);
    assert(initialized == 2);
    puts("PASS: persistent HTTPS configuration, JSON/token across POSTs, 60s cooldown for 401/403/429, error recovery");
    return 0;
}
