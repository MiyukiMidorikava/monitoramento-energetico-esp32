#pragma once

#include "esp_adc/adc_continuous.h"
#include "esp_err.h"

typedef struct {
    float tensao_rms;
    float corrente_rms;
    float corrente_pre_correcao_a; /* RMS after pair rejection, before baseline/temporal filters. */
    float potencia_w;
    float offset_tensao_counts;
    float offset_corrente_counts;
} sensores_eletricos_grandezas_t;

/* Call from the pinned owner: IRQs and task notifications belong to this task. */
esp_err_t sensores_eletricos_inicializar(adc_continuous_handle_t *adc_handle);
/* Single owner: task_aquisicao_eletrica. Outputs change only on ESP_OK.
 * ESP_ERR_NOT_FINISHED means poll again after yielding; all DMA reads use timeout 0. */
esp_err_t sensores_eletricos_ler_grandezas(adc_continuous_handle_t adc_handle,
                                          sensores_eletricos_grandezas_t *grandezas);
float sensores_eletricos_filtrar_tensao(float amostra);
float sensores_eletricos_filtrar_corrente(float amostra);
float sensores_eletricos_filtrar_potencia(float amostra);
void sensores_eletricos_zerar_filtros_corrente_potencia(void);
/* Soft deadband (SCT_ZERO_CLAMP_A): below it, corrente is forced to 0.00 A and
 * potencia_w/potencia_va/fator_potencia are zeroed together with it, so no
 * ghost active/apparent power survives a current reading close to zero. */
void sensores_eletricos_aplicar_deadband(float *corrente_rms, float *potencia_w,
                                        float *potencia_va, float *fator_potencia);
/* Consumption-only clamp: negative active power and its PF become zero.
 * RMS current/apparent power survive; small or low-PF loads are not empty. */
void sensores_eletricos_aplicar_trava_potencia_real(float *corrente_rms, float *potencia_w,
                                                   float *potencia_va, float *fator_potencia);
/* Q = sqrt(max(0, S^2 - P^2)), in VAr. Returns 0.0f whenever there is no
 * apparent power (potencia_va <= 0, i.e. no-load/deadband already zeroed it). */
float sensores_eletricos_calcular_potencia_reativa(float potencia_va, float potencia_w);
