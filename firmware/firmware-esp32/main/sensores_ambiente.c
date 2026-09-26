#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/temperature_sensor.h"

#include "dht.h"

#include "config_app.h"
#include "sensores_ambiente.h"

/* Covers the ESP32-S3 die temperature range under normal to heavy load. */
#define AMBIENTE_TEMP_INTERNA_RANGE_MIN_C 10
#define AMBIENTE_TEMP_INTERNA_RANGE_MAX_C 80

static temperature_sensor_handle_t s_temp_sensor_interno = NULL;

esp_err_t sensores_ambiente_inicializar(void)
{
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = (1ULL << DHT_PIN),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&gpio_cfg));

    temperature_sensor_config_t temp_sensor_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(
        AMBIENTE_TEMP_INTERNA_RANGE_MIN_C, AMBIENTE_TEMP_INTERNA_RANGE_MAX_C);
    ESP_ERROR_CHECK(temperature_sensor_install(&temp_sensor_cfg, &s_temp_sensor_interno));
    ESP_ERROR_CHECK(temperature_sensor_enable(s_temp_sensor_interno));

    vTaskDelay(pdMS_TO_TICKS(2000));
    return ESP_OK;
}

esp_err_t sensores_ambiente_ler(float *umidade, float *temperatura)
{
    if (umidade == NULL || temperatura == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return dht_read_float_data(DHT_TYPE_AM2301, DHT_PIN, umidade, temperatura);
}

esp_err_t sensores_ambiente_ler_temperatura_interna(float *temperatura)
{
    if (temperatura == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_temp_sensor_interno == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return temperature_sensor_get_celsius(s_temp_sensor_interno, temperatura);
}
