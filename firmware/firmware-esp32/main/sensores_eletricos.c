#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "config_app.h"
#include "sensores_eletricos.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

_Static_assert(ZMPT_SAMPLES_RMS == SCT_SAMPLES_RMS, "RMS channels must share a window");
_Static_assert(ZMPT_FILTER_WINDOW == SCT_FILTER_WINDOW, "Power and RMS filters must match");

#define ADC_WINDOW_BYTES (2 * SCT_SAMPLES_RMS * SOC_ADC_DIGI_RESULT_BYTES)
#define ADC_FRAME_BYTES (60 * SOC_ADC_DIGI_RESULT_BYTES)

/* Static storage avoids placing the DMA read buffer on the 4096-byte task stack. */
static uint8_t adc_buffer[ADC_WINDOW_BYTES];
/* Only the electrical task owns these arrays. No pointer into a DMA frame escapes. */
static uint16_t tensao_samples[ZMPT_SAMPLES_RMS];
static uint16_t corrente_samples[SCT_SAMPLES_RMS];
static bool pares_validos[SCT_SAMPLES_RMS];
_Static_assert(SCT_SAMPLES_RMS >= 3, "Impulse rejection needs three samples");
_Static_assert(sizeof(adc_digi_output_data_t) == SOC_ADC_DIGI_RESULT_BYTES,
               "Unexpected ESP32-S3 DMA result size");
static atomic_bool adc_overflow;
typedef struct {
    bool ativa;
    bool tem_tensao;
    int tensao_raw;
    unsigned pares;
    int64_t inicio_us;
} janela_adc_t;
static janela_adc_t janela;

static float zmpt_filter_buffer[ZMPT_FILTER_WINDOW] = {0};
static int zmpt_filter_index = 0;
static int zmpt_filter_count = 0;
static float sct_filter_buffer[SCT_FILTER_WINDOW] = {0};
static int sct_filter_index = 0;
static int sct_filter_count = 0;
static float potencia_filter_buffer[SCT_FILTER_WINDOW] = {0};
static int potencia_filter_index = 0;
static int potencia_filter_count = 0;

static bool amostra_impulsiva(const uint16_t *samples, unsigned k, unsigned count)
{
    /* Compare against the local median of three, with a slope-dependent guard.
     * At the edges use the two available neighbours. Never change samples in
     * place: every decision uses the original window, without phase shifts. */
    int a = samples[k == 0 ? 1 : k - 1];
    int b = samples[k == count - 1 ? count - 3 : (k == 0 ? 2 : k + 1)];
    int low = a < b ? a : b;
    int high = a > b ? a : b;
    int value = samples[k];
    int median = value < low ? low : (value > high ? high : value);
    int deviation = value > median ? value - median : median - value;
    return deviation > ELETRICA_RF_MIN_COUNTS &&
           deviation > ELETRICA_RF_SLOPE_FACTOR * (high - low);
}

static bool IRAM_ATTR adc_frame_pronto(adc_continuous_handle_t handle,
                             const adc_continuous_evt_data_t *event, void *user_data)
{
    (void)handle;
    (void)event;
    BaseType_t acordar = pdFALSE;
    vTaskNotifyGiveFromISR((TaskHandle_t)user_data, &acordar);
    return acordar == pdTRUE;
}

static bool IRAM_ATTR adc_pool_overflow(adc_continuous_handle_t handle,
                              const adc_continuous_evt_data_t *event, void *user_data)
{
    atomic_store_explicit(&adc_overflow, true, memory_order_relaxed);
    return adc_frame_pronto(handle, event, user_data);
}

esp_err_t sensores_eletricos_inicializar(adc_continuous_handle_t *adc_handle)
{
    if (adc_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    adc_continuous_handle_cfg_t init_cfg = {
        .max_store_buf_size = 2 * ADC_WINDOW_BYTES,
        .conv_frame_size = ADC_FRAME_BYTES,
    };
    esp_err_t ret = adc_continuous_new_handle(&init_cfg, adc_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    adc_digi_pattern_config_t pattern[] = {
        {.atten = ZMPT_ADC_ATTEN, .channel = ZMPT_ADC_CHANNEL,
         .unit = ZMPT_ADC_UNIT, .bit_width = ZMPT_ADC_BITWIDTH},
        {.atten = ZMPT_ADC_ATTEN, .channel = SCT_ADC_CHANNEL,
         .unit = ZMPT_ADC_UNIT, .bit_width = ZMPT_ADC_BITWIDTH},
    };
    adc_continuous_config_t config = {
        .pattern_num = 2,
        .adc_pattern = pattern,
        .sample_freq_hz = ELETRICA_ADC_SAMPLE_FREQ_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    ret = adc_continuous_config(*adc_handle, &config);
    if (ret == ESP_OK) {
        adc_continuous_evt_cbs_t callbacks = {
            .on_conv_done = adc_frame_pronto,
            .on_pool_ovf = adc_pool_overflow,
        };
        ret = adc_continuous_register_event_callbacks(*adc_handle, &callbacks, xTaskGetCurrentTaskHandle());
    }
    if (ret != ESP_OK) {
        adc_continuous_deinit(*adc_handle);
        *adc_handle = NULL;
        return ret;
    }

    return ESP_OK;
}

static esp_err_t encerrar_janela(adc_continuous_handle_t adc_handle, esp_err_t resultado)
{
    esp_err_t ret = adc_continuous_stop(adc_handle);
    if (ret == ESP_OK) {
        janela.ativa = false;
    }
    return ret == ESP_OK ? resultado : ret;
}

esp_err_t sensores_eletricos_ler_grandezas(adc_continuous_handle_t adc_handle,
                                          sensores_eletricos_grandezas_t *grandezas)
{
    if (grandezas == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (adc_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!janela.ativa) {
        esp_err_t ret = adc_continuous_flush_pool(adc_handle);
        if (ret != ESP_OK) {
            return ret;
        }
        janela = (janela_adc_t){0};
        atomic_store_explicit(&adc_overflow, false, memory_order_relaxed);
        ret = adc_continuous_start(adc_handle);
        if (ret != ESP_OK) {
            return ret;
        }
        janela.inicio_us = esp_timer_get_time();
        janela.ativa = true;
        return ESP_ERR_NOT_FINISHED;
    }

    if (esp_timer_get_time() - janela.inicio_us >= (int64_t)ELETRICA_ADC_TIMEOUT_MS * 1000) {
        return encerrar_janela(adc_handle, ESP_ERR_TIMEOUT);
    }
    if (atomic_load_explicit(&adc_overflow, memory_order_relaxed)) {
        return encerrar_janela(adc_handle, ESP_ERR_INVALID_STATE);
    }

    uint32_t bytes = 0;
    esp_err_t ret = adc_continuous_read(adc_handle, (uint8_t *)adc_buffer,
                                        sizeof(adc_buffer), &bytes, 0);
    if (atomic_load_explicit(&adc_overflow, memory_order_relaxed)) {
        return encerrar_janela(adc_handle, ESP_ERR_INVALID_STATE);
    }
    if (ret == ESP_ERR_TIMEOUT) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (ret != ESP_OK) {
        return encerrar_janela(adc_handle, ret);
    }
    if (bytes > sizeof(adc_buffer) || bytes % SOC_ADC_DIGI_RESULT_BYTES != 0) {
        return encerrar_janela(adc_handle, ESP_ERR_INVALID_RESPONSE);
    }

    for (uint32_t k = 0; k < bytes / SOC_ADC_DIGI_RESULT_BYTES; ++k) {
        adc_digi_output_data_t sample;
        /* TYPE2 on S3 has a reserved bit between data and channel. Use the SDK
         * bitfields, not hand-coded shifts or a cast of an unaligned buffer. */
        memcpy(&sample, &adc_buffer[k * SOC_ADC_DIGI_RESULT_BYTES], sizeof(sample));
        if (sample.type2.unit != ZMPT_ADC_UNIT) {
            return encerrar_janela(adc_handle, ESP_ERR_INVALID_RESPONSE);
        }
        if (sample.type2.channel == ZMPT_ADC_CHANNEL && !janela.tem_tensao) {
            janela.tensao_raw = sample.type2.data;
            janela.tem_tensao = true;
        } else if (sample.type2.channel == SCT_ADC_CHANNEL && janela.tem_tensao) {
            tensao_samples[janela.pares] = (uint16_t)janela.tensao_raw;
            corrente_samples[janela.pares] = (uint16_t)sample.type2.data;
            janela.tem_tensao = false;
            if (++janela.pares == SCT_SAMPLES_RMS) {
                break;
            }
        } else {
            return encerrar_janela(adc_handle, ESP_ERR_INVALID_RESPONSE);
        }
    }
    if (janela.pares < SCT_SAMPLES_RMS) {
        return ESP_ERR_NOT_FINISHED;
    }
    ret = encerrar_janela(adc_handle, ESP_OK);
    if (ret != ESP_OK) {
        return ret;
    }
    if (atomic_load_explicit(&adc_overflow, memory_order_relaxed)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Reject the ENTIRE pair when either channel has an isolated RF impulse.
     * V RMS, I RMS and mean(v*i) always use the same time indices/count. */
    unsigned validos = 0;
    for (unsigned k = 0; k < janela.pares; ++k) {
        pares_validos[k] = !amostra_impulsiva(tensao_samples, k, janela.pares) &&
                          !amostra_impulsiva(corrente_samples, k, janela.pares);
        validos += pares_validos[k] ? 1u : 0u;
    }
    if (validos == 0 || (janela.pares - validos) * 100u > janela.pares * ELETRICA_RF_MAX_REJECT_PCT) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Exact DC means of the retained samples, then explicit centering. */
    uint64_t soma_v = 0;
    uint64_t soma_i = 0;
    for (unsigned k = 0; k < janela.pares; ++k) {
        if (!pares_validos[k]) {
            continue;
        }
        soma_v += tensao_samples[k];
        soma_i += corrente_samples[k];
    }
    double media_v = (double)soma_v / validos;
    double media_i = (double)soma_i / validos;
    double soma_vv = 0.0;
    double soma_ii = 0.0;
    double soma_vi = 0.0;
    for (unsigned k = 0; k < janela.pares; ++k) {
        if (!pares_validos[k]) {
            continue;
        }
        double v = (double)tensao_samples[k] - media_v;
        double i = (double)corrente_samples[k] - media_i;
        soma_vv += v * v;
        soma_ii += i * i;
        soma_vi += v * i;
    }
    double var_v = soma_vv / validos;
    double var_i = soma_ii / validos;
    double ganho_v = (ZMPT_ADC_VREF / 4095.0) * ZMPT_CALIBRATION_GAIN;
    double ganho_i = (SCT_ADC_VREF / 4095.0) * SCT_CALIBRATION_GAIN_A_PER_V;
    sensores_eletricos_grandezas_t resultado = {
        .tensao_rms = (float)(sqrt(var_v) * ganho_v),
        /* Mean of v[n]*i[n] after removing each channel's DC offset. */
        .potencia_w = (float)((soma_vi / validos) * ganho_v * ganho_i),
        .offset_tensao_counts = (float)media_v,
        .offset_corrente_counts = (float)media_i,
    };
    float i_bruta = (float)(sqrt(var_i) * ganho_i);
    resultado.corrente_pre_correcao_a = i_bruta;
    float i_sq = (i_bruta * i_bruta) - (SCT_BASELINE_CURRENT_A * SCT_BASELINE_CURRENT_A);
    /* Baseline is removed exactly once. The current clamp belongs after filtering. */
    resultado.corrente_rms = (i_sq > 0.0f) ? sqrtf(i_sq) : 0.0f;
    if (resultado.corrente_rms == 0.0f) {
        resultado.potencia_w = 0.0f;
    }
    if (resultado.tensao_rms < ZMPT_ZERO_CLAMP_V) {
        resultado.tensao_rms = 0.0f;
    }
    *grandezas = resultado;
    return ESP_OK;
}

float sensores_eletricos_filtrar_tensao(float amostra)
{
    zmpt_filter_buffer[zmpt_filter_index] = amostra;
    zmpt_filter_index = (zmpt_filter_index + 1) % ZMPT_FILTER_WINDOW;

    if (zmpt_filter_count < ZMPT_FILTER_WINDOW) {
        zmpt_filter_count++;
    }

    float soma = 0.0f;
    for (int i = 0; i < zmpt_filter_count; i++) {
        soma += zmpt_filter_buffer[i];
    }

    return soma / zmpt_filter_count;
}

float sensores_eletricos_filtrar_corrente(float amostra)
{
    sct_filter_buffer[sct_filter_index] = amostra;
    sct_filter_index = (sct_filter_index + 1) % SCT_FILTER_WINDOW;

    if (sct_filter_count < SCT_FILTER_WINDOW) {
        sct_filter_count++;
    }

    float soma = 0.0f;
    for (int i = 0; i < sct_filter_count; i++) {
        soma += sct_filter_buffer[i];
    }

    return soma / sct_filter_count;
}

float sensores_eletricos_filtrar_potencia(float amostra)
{
    potencia_filter_buffer[potencia_filter_index] = amostra;
    potencia_filter_index = (potencia_filter_index + 1) % SCT_FILTER_WINDOW;
    if (potencia_filter_count < SCT_FILTER_WINDOW) {
        potencia_filter_count++;
    }
    float soma = 0.0f;
    for (int i = 0; i < potencia_filter_count; i++) {
        soma += potencia_filter_buffer[i];
    }
    return soma / potencia_filter_count;
}

void sensores_eletricos_zerar_filtros_corrente_potencia(void)
{
    /* Do not let a previously loaded window leak power into an empty one. */
    memset(sct_filter_buffer, 0, sizeof(sct_filter_buffer));
    memset(potencia_filter_buffer, 0, sizeof(potencia_filter_buffer));
    sct_filter_index = sct_filter_count = 0;
    potencia_filter_index = potencia_filter_count = 0;
}

void sensores_eletricos_aplicar_deadband(float *corrente_rms, float *potencia_w,
                                        float *potencia_va, float *fator_potencia)
{
    if (corrente_rms == NULL || *corrente_rms >= SCT_ZERO_CLAMP_A) {
        return;
    }
    *corrente_rms = 0.0f;
    if (potencia_w != NULL) { *potencia_w = 0.0f; }
    if (potencia_va != NULL) { *potencia_va = 0.0f; }
    if (fator_potencia != NULL) { *fator_potencia = 0.0f; }
}

void sensores_eletricos_aplicar_trava_potencia_real(float *corrente_rms, float *potencia_w,
                                                   float *potencia_va, float *fator_potencia)
{
    if (corrente_rms == NULL || potencia_w == NULL) {
        return;
    }

    /* Consumption-only meter: phase noise must never publish negative P.
     * Preserve measured RMS/apparent power; low-PF and sub-8 W loads exist. */
    (void)potencia_va;
    if (*potencia_w < 0.0f) {
        *potencia_w = 0.0f;
        if (fator_potencia != NULL) { *fator_potencia = 0.0f; }
    }
}

float sensores_eletricos_calcular_potencia_reativa(float potencia_va, float potencia_w)
{
    if (potencia_va <= 0.0f) {
        return 0.0f;
    }
    return sqrtf(fmaxf(0.0f, (potencia_va * potencia_va) - (potencia_w * potencia_w)));
}
