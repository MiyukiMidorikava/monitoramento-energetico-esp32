#pragma once

#include "sdkconfig.h"

#ifndef CONFIG_MONITOR_WIFI_SSID
#define CONFIG_MONITOR_WIFI_SSID "SEU_SSID"
#endif

#ifndef CONFIG_MONITOR_WIFI_PASS
#define CONFIG_MONITOR_WIFI_PASS "SUA_SENHA"
#endif

#ifndef CONFIG_MONITOR_FLASK_ENDPOINT_URL
#define CONFIG_MONITOR_FLASK_ENDPOINT_URL "https://energia.example.invalid/telemetria"
#endif

#ifndef CONFIG_MONITOR_API_TOKEN
#define CONFIG_MONITOR_API_TOKEN ""
#endif

#define API_TOKEN CONFIG_MONITOR_API_TOKEN

#define DHT_PIN      4
#define ZMPT_PIN     5
#define SCT_PIN      6

#define I2C_SDA_PIN  8
#define I2C_SCL_PIN  9

#define I2C_PORT     I2C_NUM_0
#define I2C_FREQ_HZ  400000

#define WIFI_SSID             CONFIG_MONITOR_WIFI_SSID
#define WIFI_PASS             CONFIG_MONITOR_WIFI_PASS
#define FLASK_ENDPOINT_URL    CONFIG_MONITOR_FLASK_ENDPOINT_URL

#define DS3231_ADDR      0x68
#define DS3231_REG_TIME  0x00
#define DS3231_REG_HOUR  0x02

#define ZMPT_ADC_UNIT          ADC_UNIT_1
#define ZMPT_ADC_CHANNEL       ADC_CHANNEL_4
#define ZMPT_ADC_ATTEN         ADC_ATTEN_DB_12
#define ZMPT_ADC_BITWIDTH      ADC_BITWIDTH_12
#define ZMPT_SAMPLES_RMS       600
#define ZMPT_FILTER_WINDOW     8
#define ZMPT_ADC_VREF          3.3f
/* Bench calibration 2026-09-22, multimeter reference: ESP32 read 135.0 V for
 * an actual 122.4 V, so the prior gain (700.0f) is scaled by
 * Fator_V = 122.4 / 135.0 = 0.906667. */
#define ZMPT_CALIBRATION_GAIN  634.667f
#define ZMPT_ZERO_CLAMP_V      15.0f

#define SCT_ADC_CHANNEL                ADC_CHANNEL_5
#define SCT_SAMPLES_RMS                600
#define SCT_FILTER_WINDOW              8
#define SCT_ADC_VREF                   3.3f
/* Bench calibration 2026-09-22, multimeter reference: ESP32 read 9.22 A
 * continuous for an actual ~7.15 A (6.6-7.7 A meter range), so
 * Fator_I = 7.15 / 9.22 = 0.775489 scales both the gain and the quadratic
 * baseline below by the same factor -- the baseline is a fixed ADC/self-noise
 * offset expressed in the same amp domain as the gained RMS current, so it
 * must shrink with the gain to keep canceling exactly and preserve the
 * already-correct 0.00 A no-load reading. */
#define SCT_CALIBRATION_GAIN_A_PER_V   77.549f
#define SCT_BASELINE_CURRENT_A         1.2020f
/* Residual-current deadband AFTER the single quadratic baseline removal.
 * 20 mA is a software cutoff, not a claim of ADC accuracy: calibrate with
 * no load and known small loads. Do not classify all signals below 2.45 A
 * raw, 8 W, or a low power factor as empty: real loads can occupy that range.
 * RF rejection is handled by the sample-pair and temporal filters. */
#define SCT_ZERO_CLAMP_A               0.02f

/* Selective median: reject isolated impulses, not normal waveform slopes. */
#define ELETRICA_RF_MIN_COUNTS         32
#define ELETRICA_RF_SLOPE_FACTOR       6
#define ELETRICA_RF_MAX_REJECT_PCT     10

/* Cross-cycle impulse rejection: a pure median of the last ELETRICA_IMPULSO_TAM
 * acquisition cycles (batched together before the next HTTP dispatch) ignores
 * up to (ELETRICA_IMPULSO_TAM - 1) / 2 consecutive Wi-Fi TX burst samples. */
#define ELETRICA_IMPULSO_TAM                5
_Static_assert(ELETRICA_IMPULSO_TAM % 2 == 1, "Pure median needs an odd window");

/* Total conversions: alternating voltage/current, 600 pairs in 100 ms. */
#define ELETRICA_ADC_SAMPLE_FREQ_HZ    12000
#define ELETRICA_ADC_POLL_MS           10
#define ELETRICA_ADC_TIMEOUT_MS        300

/* Fixed floor for the OTA polling period, independent of the configurable
 * MONITOR_OTA_INTERVAL_SECONDS: a short Kconfig value left over from bench
 * testing must not turn into a tight retry loop that floods the serial log
 * and Wi-Fi traffic during an overnight stress run. Applies only after an
 * actual check was attempted (success, failure, or same-image skip). */
#define OTA_TASK_INTERVAL_MS    1800000

/* Short retry while Wi-Fi isn't up yet, so a boot-time disconnect or a
 * transient outage doesn't cost up to 30 extra minutes before the first
 * real OTA check -- no download is attempted here, so retrying often is
 * cheap. */
#define OTA_TASK_WIFI_RETRY_MS  10000

#define OLED_ALTURA_PIXELS  64
#define OLED_FONTE_PIXELS    16
#define OLED_MAX_COLUNAS     21

#define WIFI_CONNECTED_BIT (1 << 0)
#define WIFI_FAIL_BIT      (1 << 1)

/* Ceiling for the wait for a Wi-Fi IP before attempting the boot NTP sync;
 * past this the DS3231 fallback runs instead of stalling the ambiente task. */
#define APP_NTP_WIFI_WAIT_MS    20000

/* A boot-time NTP failure (or ordinary RTC drift over weeks of uptime) must
 * not stick for the device's entire runtime -- retry roughly every 6 hours
 * whenever Wi-Fi is up. */
#define APP_NTP_RESYNC_INTERVAL_MS  21600000

#define APP_TASK_PERIOD_ELETRICA_MS   500
#define APP_TASK_PERIOD_AMBIENTE_MS  3000
#define APP_TASK_PERIOD_ENVIO_MS      500
#define APP_TASK_PERIOD_DISPLAY_MS    3500
#define APP_HTTP_RETRY_INITIAL_MS     5000
#define APP_HTTP_RETRY_MAX_MS         10000

/* Allocate the GDMA ISR on the electrical core; keep DHT critical sections away. */
#define APP_TASK_CORE_ELETRICA          1
#define APP_TASK_CORE_AMBIENTE          0
#define APP_TASK_CORE_REDE              0
