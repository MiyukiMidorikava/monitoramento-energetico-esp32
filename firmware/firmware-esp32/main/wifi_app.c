#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "config_app.h"
#include "wifi_app.h"

static const char *TAG = "wifi_app";

static TaskHandle_t reconnect_task;

static void wifi_reconnect_task(void *arg)
{
    app_shared_data_t *shared = arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Never delay the event loop. Retry indefinitely with bounded jitter. */
        vTaskDelay(pdMS_TO_TICKS(5000 + esp_random() % 1000));
        if (wifi_app_esta_conectado(shared)) continue;
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Falha ao iniciar conexao Wi-Fi: %s", esp_err_to_name(err));
            xTaskNotifyGive(reconnect_task);
        }
        /* A failed association produces DISCONNECTED and schedules another try. */
    }
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    app_shared_data_t *shared = arg;
    if (shared == NULL || shared->wifi_event_group == NULL) return;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xTaskNotifyGive(reconnect_task);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(shared->wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(shared->wifi_event_group, WIFI_FAIL_BIT);
        xTaskNotifyGive(reconnect_task);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "Wi-Fi conectado. IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupClearBits(shared->wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupSetBits(shared->wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_app_inicializar_sta(app_shared_data_t *shared)
{
    if (shared == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = nvs_flash_init();
    /* Never erase the meter's accumulated energy as Wi-Fi recovery. */
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    shared->wifi_event_group = xEventGroupCreate();
    if (shared->wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        shared,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        shared,
                                                        NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    if (xTaskCreate(wifi_reconnect_task, "wifi_reconnect", 3072, shared,
                    2, &reconnect_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Driver initialized; AP availability must not gate ADC task startup. */
    return ESP_OK;
}

bool wifi_app_esta_conectado(const app_shared_data_t *shared)
{
    if (shared == NULL || shared->wifi_event_group == NULL) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(shared->wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}
