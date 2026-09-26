#include <stdbool.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"

#include "config_app.h"
#include "tempo.h"

static const char *TAG = "tempo";

static uint8_t bcd_para_dec(uint8_t bcd)
{
    return (uint8_t)((bcd >> 4) * 10 + (bcd & 0x0F));
}

static uint8_t dec_para_bcd(uint8_t dec)
{
    return (uint8_t)(((dec / 10) << 4) | (dec % 10));
}

static uint8_t ds3231_hora_reg_para_24h(uint8_t hora_reg)
{
    if ((hora_reg & 0x40) == 0) {
        return bcd_para_dec(hora_reg & 0x3F);
    }

    uint8_t hora12 = bcd_para_dec(hora_reg & 0x1F);
    bool is_pm = (hora_reg & 0x20) != 0;

    if (hora12 == 12) {
        return is_pm ? 12 : 0;
    }

    return is_pm ? (uint8_t)(hora12 + 12) : hora12;
}

static esp_err_t ds3231_ler_tempo(const app_handles_t *handles, struct tm *tempo)
{
    if (handles == NULL || tempo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handles->ds3231_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buffer[7] = {0};
    uint8_t reg = DS3231_REG_TIME;
    esp_err_t ret = i2c_master_transmit_receive(handles->ds3231_dev,
                                                &reg,
                                                sizeof(reg),
                                                buffer,
                                                sizeof(buffer),
                                                200);
    if (ret != ESP_OK) {
        return ret;
    }

    tempo->tm_sec = bcd_para_dec(buffer[0] & 0x7F);
    tempo->tm_min = bcd_para_dec(buffer[1] & 0x7F);
    tempo->tm_hour = ds3231_hora_reg_para_24h(buffer[2]);
    tempo->tm_mday = bcd_para_dec(buffer[4] & 0x3F);
    tempo->tm_mon = bcd_para_dec(buffer[5] & 0x1F) - 1;
    tempo->tm_year = bcd_para_dec(buffer[6]) + 100;

    return ESP_OK;
}

static esp_err_t ds3231_escrever_tempo_24h(const app_handles_t *handles, const struct tm *tempo)
{
    if (handles == NULL || tempo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handles->ds3231_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (tempo->tm_hour < 0 || tempo->tm_hour > 23 ||
        tempo->tm_min < 0 || tempo->tm_min > 59 ||
        tempo->tm_sec < 0 || tempo->tm_sec > 59 ||
        tempo->tm_mday < 1 || tempo->tm_mday > 31 ||
        tempo->tm_mon < 0 || tempo->tm_mon > 11 ||
        tempo->tm_year < 100 || tempo->tm_year > 199) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t dia_semana_ds3231 = (uint8_t)(tempo->tm_wday == 0 ? 7 : tempo->tm_wday);
    uint8_t payload[8] = {
        DS3231_REG_TIME,
        dec_para_bcd((uint8_t)tempo->tm_sec),
        dec_para_bcd((uint8_t)tempo->tm_min),
        dec_para_bcd((uint8_t)tempo->tm_hour),
        dec_para_bcd(dia_semana_ds3231),
        dec_para_bcd((uint8_t)tempo->tm_mday),
        dec_para_bcd((uint8_t)(tempo->tm_mon + 1)),
        dec_para_bcd((uint8_t)(tempo->tm_year - 100)),
    };

    return i2c_master_transmit(handles->ds3231_dev, payload, sizeof(payload), 200);
}

static esp_err_t ds3231_forcar_modo_24h(const app_handles_t *handles)
{
    if (handles == NULL || handles->ds3231_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg = DS3231_REG_HOUR;
    uint8_t hora_reg = 0;

    esp_err_t ret = i2c_master_transmit_receive(handles->ds3231_dev,
                                                &reg,
                                                sizeof(reg),
                                                &hora_reg,
                                                sizeof(hora_reg),
                                                200);
    if (ret != ESP_OK) {
        return ret;
    }

    if ((hora_reg & 0x40) == 0) {
        return ESP_OK;
    }

    uint8_t hora24 = ds3231_hora_reg_para_24h(hora_reg);
    uint8_t payload[2] = {DS3231_REG_HOUR, dec_para_bcd(hora24)};
    return i2c_master_transmit(handles->ds3231_dev, payload, sizeof(payload), 200);
}

static esp_err_t ds3231_sincronizar_ntp_boot_24h(const app_handles_t *handles)
{
    setenv("TZ", "UTC0", 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);

    esp_err_t sync_res = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
    if (sync_res != ESP_OK) {
        esp_netif_sntp_deinit();
        return sync_res;
    }

    time_t agora_epoch = 0;
    time(&agora_epoch);
    if (agora_epoch <= 0) {
        esp_netif_sntp_deinit();
        return ESP_FAIL;
    }

    struct tm agora_brasilia = {0};
    if (localtime_r(&agora_epoch, &agora_brasilia) == NULL) {
        esp_netif_sntp_deinit();
        return ESP_FAIL;
    }

    esp_err_t ret = ds3231_escrever_tempo_24h(handles, &agora_brasilia);
    esp_netif_sntp_deinit();
    return ret;
}

static esp_err_t sistema_ajustar_tempo_a_partir_do_ds3231(const app_handles_t *handles)
{
    struct tm tempo_rtc = {0};
    esp_err_t rtc_res = ds3231_ler_tempo(handles, &tempo_rtc);
    if (rtc_res != ESP_OK) {
        return rtc_res;
    }

    tempo_rtc.tm_isdst = 0;
    time_t epoch = mktime(&tempo_rtc);
    if (epoch < 0) {
        return ESP_FAIL;
    }

    struct timeval tv = {
        .tv_sec = epoch,
        .tv_usec = 0,
    };

    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t tempo_inicializar_i2c_rtc(app_handles_t *handles)
{
    if (handles == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_config_t i2c_bus_config = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &handles->i2c_bus));

    i2c_device_config_t rtc_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(handles->i2c_bus, &rtc_cfg, &handles->ds3231_dev));
    return ESP_OK;
}

esp_err_t tempo_sincronizar_boot(app_handles_t *handles, app_shared_data_t *shared)
{
    if (handles == NULL || shared == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t rtc_mode_res = ds3231_forcar_modo_24h(handles);
    if (rtc_mode_res != ESP_OK) {
        ESP_LOGW(TAG, "RTC indisponivel: %s", esp_err_to_name(rtc_mode_res));
    }
    esp_err_t rtc_sync_res = ds3231_sincronizar_ntp_boot_24h(handles);
    if (rtc_sync_res == ESP_OK) {
        shared->boot_sync_source = "NTP";
        ESP_LOGI(TAG, "RTC sincronizado via NTP no boot (UTC, 24h)");
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "Falha ao sincronizar RTC via NTP no boot: %s",
             esp_err_to_name(rtc_sync_res));

    esp_err_t fallback_res = sistema_ajustar_tempo_a_partir_do_ds3231(handles);
    if (fallback_res == ESP_OK) {
        shared->boot_sync_source = "DS3231";
        ESP_LOGI(TAG, "Relogio do sistema ajustado a partir do DS3231 (fallback)");
        return ESP_OK;
    }

    shared->boot_sync_source = "NONE";
    ESP_LOGW(TAG,
             "Falha no fallback DS3231 para relogio do sistema: %s",
             esp_err_to_name(fallback_res));
    return fallback_res;
}

esp_err_t tempo_resincronizar_ntp(app_handles_t *handles)
{
    if (handles == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ds3231_sincronizar_ntp_boot_24h(handles);
}

esp_err_t tempo_ler_sistema(struct tm *tempo)
{
    if (tempo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t agora = 0;
    time(&agora);
    if (agora <= 0) {
        return ESP_FAIL;
    }

    if (localtime_r(&agora, tempo) == NULL) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t tempo_ler_ds3231(const app_handles_t *handles, struct tm *tempo)
{
    return ds3231_ler_tempo(handles, tempo);
}
