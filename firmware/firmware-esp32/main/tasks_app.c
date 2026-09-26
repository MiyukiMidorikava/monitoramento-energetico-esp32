#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "config_app.h"
#include "display.h"
#include "http_client_app.h"
#include "sensores_ambiente.h"
#include "sensores_eletricos.h"
#include "tasks_app.h"
#include "telemetria.h"
#include "tempo.h"
#include "ota_app.h"
#include "wifi_app.h"

static const char *TAG = "tasks_app";

_Static_assert(APP_TASK_CORE_ELETRICA != APP_TASK_CORE_AMBIENTE,
               "DHT must not mask interrupts on the GDMA core");
_Static_assert(APP_TASK_CORE_ELETRICA < portNUM_PROCESSORS, "Dual-core ESP32-S3 required");

/* Cross-cycle impulse rejection for corrente_rms/potencia_w: the 8-sample
 * moving average in sensores_eletricos.c smooths steady-state noise but
 * still lets Wi-Fi TX burst noise (0.6-1.1 A observed with no load) bleed
 * through for several cycles. A pure median over the last ELETRICA_IMPULSO_TAM
 * readings ignores that noise outright before it reaches shared_data. */
typedef struct {
    float valores[ELETRICA_IMPULSO_TAM];
    int indice;
    int contagem;
} buffer_impulso_t;

static buffer_impulso_t buffer_impulso_corrente;
static buffer_impulso_t buffer_impulso_potencia;

static void limpar_buffer_impulso(buffer_impulso_t *buf)
{
    memset(buf, 0, sizeof(*buf));
}

/* Mediana pura das ultimas ELETRICA_IMPULSO_TAM amostras: ordena o buffer e
 * devolve exatamente o elemento central (indice 2 de 5), sem nenhuma media.
 * Ate (ELETRICA_IMPULSO_TAM - 1) / 2 amostras consecutivas contaminadas por
 * rajadas de transmissao Wi-Fi ficam nas extremidades da ordenacao e nunca
 * chegam a influenciar o valor publicado. */
static float filtrar_mediana(buffer_impulso_t *buf, float nova_amostra)
{
    buf->valores[buf->indice] = nova_amostra;
    buf->indice = (buf->indice + 1) % ELETRICA_IMPULSO_TAM;
    if (buf->contagem < ELETRICA_IMPULSO_TAM) {
        buf->contagem++;
    }

    float ordenado[ELETRICA_IMPULSO_TAM];
    memcpy(ordenado, buf->valores, sizeof(float) * (size_t)buf->contagem);
    for (int i = 1; i < buf->contagem; i++) {
        float chave = ordenado[i];
        int j = i - 1;
        while (j >= 0 && ordenado[j] > chave) {
            ordenado[j + 1] = ordenado[j];
            j--;
        }
        ordenado[j + 1] = chave;
    }

    return ordenado[buf->contagem / 2];
}

static bool runtime_lock(app_runtime_t *runtime)
{
    if (runtime == NULL || runtime->mutex == NULL) {
        return false;
    }

    return xSemaphoreTake(runtime->mutex, pdMS_TO_TICKS(100)) == pdTRUE;
}

static void runtime_unlock(app_runtime_t *runtime)
{
    if (runtime != NULL && runtime->mutex != NULL) {
        xSemaphoreGive(runtime->mutex);
    }
}

static bool runtime_snapshot(app_runtime_t *runtime,
                             app_medicoes_t *medicoes,
                             app_shared_data_t *shared)
{
    if (runtime == NULL || medicoes == NULL || shared == NULL) {
        return false;
    }

    if (!runtime_lock(runtime)) {
        return false;
    }

    *medicoes = runtime->medicoes;
    *shared = runtime->shared;

    runtime_unlock(runtime);
    app_eletrica_snapshot_t eletrica;
    if (shared_data_get_medicoes(&eletrica) != ESP_OK) {
        return false;
    }
    {
        int64_t age_us = esp_timer_get_time() - eletrica.t_leitura_us;
        /* At most three acquisition periods; reject startup and stalled ADC. */
        bool fresh = eletrica.t_leitura_us > 0 && age_us >= 0 &&
                     age_us <= 3LL * APP_TASK_PERIOD_ELETRICA_MS * 1000;
        medicoes->zmpt_res = fresh ? eletrica.resultado : ESP_ERR_INVALID_STATE;
        medicoes->sct_res = medicoes->zmpt_res;
        medicoes->tensao_rms = eletrica.tensao_rms;
        medicoes->corrente_rms = eletrica.corrente_rms;
        medicoes->corrente_pre_correcao_a = eletrica.corrente_pre_correcao_a;
        medicoes->potencia_w = eletrica.potencia_w;
        medicoes->potencia_va = eletrica.potencia_va;
        medicoes->potencia_reativa_var = eletrica.potencia_reativa_var;
        medicoes->fator_potencia = eletrica.fator_potencia;
        medicoes->energia_kwh = eletrica.energia_kwh;
    }
    return true;
}

static void task_aquisicao_eletrica(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    /* GDMA allocates its interrupt on the calling core. Create and operate the
     * driver here, after xTaskCreatePinnedToCore, with this task as sole owner. */
    adc_continuous_handle_t adc_handle = NULL;
    app_eletrica_snapshot_t publicacao = {.resultado = ESP_ERR_INVALID_STATE};
    /* Restored before task creation; retain it even if the first ADC read fails. */
    ESP_ERROR_CHECK(shared_data_get_medicoes(&publicacao));
    double energia_kwh = publicacao.energia_kwh;
    int64_t leitura_anterior_us = 0;
    double potencia_anterior_w = 0.0;
    bool tem_leitura_anterior = false;

    /* Reset only on a completed cycle below, never inside the
     * ESP_ERR_NOT_FINISHED poll-and-continue branch: a genuinely stalled ADC
     * still yields every ELETRICA_ADC_POLL_MS, so the idle-task-starvation
     * watchdog alone would never catch it. */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    while (1) {
        sensores_eletricos_grandezas_t amostra = {0};
        esp_err_t leitura_res = ESP_OK;
        if (adc_handle == NULL) {
            leitura_res = sensores_eletricos_inicializar(&adc_handle);
        }
        if (leitura_res == ESP_OK) {
            leitura_res = sensores_eletricos_ler_grandezas(adc_handle, &amostra);
        }
        if (leitura_res == ESP_ERR_NOT_FINISHED) {
            /* DMA IRQ wakes the owner directly, including while core 0 runs
             * the DHT critical section. Timeout also detects a stalled ADC. */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ELETRICA_ADC_POLL_MS));
            continue;
        }

        int64_t agora_us = esp_timer_get_time();
        float tensao_rms = 0.0f;
        float corrente_rms = 0.0f;
        float potencia_w = 0.0f;
        float potencia_va = 0.0f;
        float potencia_reativa_var = 0.0f;
        float fator_potencia = 0.0f;
        if (leitura_res == ESP_OK) {
            tensao_rms = sensores_eletricos_filtrar_tensao(amostra.tensao_rms);
            bool em_vazio = amostra.corrente_rms < SCT_ZERO_CLAMP_A;
            if (em_vazio) {
                sensores_eletricos_zerar_filtros_corrente_potencia();
                limpar_buffer_impulso(&buffer_impulso_corrente);
                limpar_buffer_impulso(&buffer_impulso_potencia);
            }
            corrente_rms = sensores_eletricos_filtrar_corrente(em_vazio ? 0.0f : amostra.corrente_rms);
            potencia_w = sensores_eletricos_filtrar_potencia(em_vazio ? 0.0f : amostra.potencia_w);

            /* Pure median of the last ELETRICA_IMPULSO_TAM acquisition cycles:
             * up to two consecutive Wi-Fi TX burst samples are ignored outright. */
            corrente_rms = filtrar_mediana(&buffer_impulso_corrente, corrente_rms);
            potencia_w = filtrar_mediana(&buffer_impulso_potencia, potencia_w);

            /* The baseline was already removed by the sensor, before filtering. */
            if (em_vazio) {
                corrente_rms = 0.0f;
                potencia_w = 0.0f;
            }
            if (tensao_rms < ZMPT_ZERO_CLAMP_V) {
                tensao_rms = 0.0f;
            }
            potencia_va = tensao_rms * corrente_rms;
            if (potencia_va > 0.0f) {
                fator_potencia = potencia_w / potencia_va;
            } else {
                potencia_w = 0.0f;
            }
            /* Soft deadband: zeroes corrente/potencia_w/potencia_va/fator_potencia
             * together when the filtered current is below SCT_ZERO_CLAMP_A. */
            sensores_eletricos_aplicar_deadband(&corrente_rms, &potencia_w, &potencia_va, &fator_potencia);
            /* Clamp phase-noise negative active power, preserving real small loads. */
            sensores_eletricos_aplicar_trava_potencia_real(&corrente_rms, &potencia_w, &potencia_va, &fator_potencia);
            potencia_reativa_var = sensores_eletricos_calcular_potencia_reativa(potencia_va, potencia_w);

            /* Trapezoidal integration between consecutive valid samples,
             * including load-to-zero transitions. Monotonic microseconds are
             * independent of RTC/NTP corrections; never bridge an ADC failure. */
            if (tem_leitura_anterior && agora_us > leitura_anterior_us) {
                double dt_us = (double)(agora_us - leitura_anterior_us);
                energia_kwh += 0.5 * (potencia_anterior_w + (double)potencia_w) * dt_us / 3.6e12;
            }
            potencia_anterior_w = (double)potencia_w;
            energia_kwh = fmax(0.0, energia_kwh);
            leitura_anterior_us = agora_us;
            tem_leitura_anterior = true;

            publicacao.tensao_rms = tensao_rms;
            publicacao.corrente_rms = corrente_rms;
            publicacao.corrente_pre_correcao_a = amostra.corrente_pre_correcao_a;
            publicacao.potencia_w = potencia_w;
            publicacao.potencia_va = potencia_va;
            publicacao.potencia_reativa_var = potencia_reativa_var;
            publicacao.fator_potencia = fator_potencia;
            publicacao.energia_kwh = energia_kwh;
            publicacao.t_leitura_us = agora_us;

            ESP_LOGI("ELETRICA", "vrms_v=%.2f irms_a=%.3f p_w=%.2f s_va=%.2f q_var=%.2f fp=%.3f energia_kwh=%.6f",
                     tensao_rms, corrente_rms,
                     potencia_w, potencia_va, potencia_reativa_var, fator_potencia, energia_kwh);
        } else {
            /* Never integrate across an interval whose measurements are missing. */
            tem_leitura_anterior = false;
            ESP_LOGW("ELETRICA", "Janela ADC invalida: %s; ultima leitura preservada, energia pausada",
                     esp_err_to_name(leitura_res));
        }

        /* Publish only the completed value; never hold the mutex during ADC work. */
        publicacao.resultado = leitura_res;
        if (shared_data_set_medicoes(&publicacao) != ESP_OK) {
            ESP_LOGW(TAG, "Timeout ao publicar medicao eletrica");
        }

        esp_task_wdt_reset();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(APP_TASK_PERIOD_ELETRICA_MS));
    }
}

static void task_aquisicao_ambiente_tempo(void *arg)
{
    app_runtime_t *runtime = (app_runtime_t *)arg;
    /* ADC already runs independently while boot time synchronization waits.
     * SNTP needs a live IP to resolve/reach the NTP pool; without this wait
     * it starts right at task creation, well before Wi-Fi associates, and
     * always times out into the DS3231 fallback. */
    if (runtime->shared.wifi_event_group != NULL) {
        xEventGroupWaitBits(runtime->shared.wifi_event_group,
                            WIFI_CONNECTED_BIT,
                            pdFALSE,
                            pdTRUE,
                            pdMS_TO_TICKS(APP_NTP_WIFI_WAIT_MS));
    }
    app_shared_data_t boot_shared = {.boot_sync_source = "NONE"};
    esp_err_t sync_res = tempo_sincronizar_boot(&runtime->handles, &boot_shared);
    if (sync_res != ESP_OK) {
        ESP_LOGW(TAG, "Relogio indisponivel no boot: %s", esp_err_to_name(sync_res));
    }
    if (runtime_lock(runtime)) {
        runtime->shared.boot_sync_source = boot_shared.boot_sync_source;
        runtime_unlock(runtime);
    }
    TickType_t last_wake = xTaskGetTickCount();
    int64_t proxima_resincronizacao_us = esp_timer_get_time() +
                                          (int64_t)APP_NTP_RESYNC_INTERVAL_MS * 1000;

    /* Subscribed only after the boot sync above: that call can legitimately
     * block up to ~35 s (Wi-Fi wait plus the SNTP timeout), far past the 5 s
     * global watchdog timeout. The periodic resync below has the same issue
     * and is excluded from coverage around that one call instead. */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    while (1) {
        float umidade = 0.0f;
        float temperatura_dht = 0.0f;
        struct tm rtc_tempo = {0};
        const char *ts_source = "SYSTEM";

        if (esp_timer_get_time() >= proxima_resincronizacao_us) {
            esp_err_t resync_res = ESP_FAIL;
            if (wifi_app_esta_conectado(&runtime->shared)) {
                /* Can block up to the ~15 s SNTP timeout; unsubscribe for
                 * its duration rather than let a slow-but-healthy resync
                 * trip the watchdog. */
                esp_task_wdt_delete(NULL);
                resync_res = tempo_resincronizar_ntp(&runtime->handles);
                ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
                if (resync_res != ESP_OK) {
                    ESP_LOGW(TAG, "Resincronizacao NTP periodica falhou: %s",
                             esp_err_to_name(resync_res));
                }
            }
            /* On success, wait the full interval again; otherwise (no Wi-Fi
             * or a failed sync) retry sooner instead of losing up to another
             * full APP_NTP_RESYNC_INTERVAL_MS to a rescheduling that never
             * actually attempted anything. */
            uint32_t proximo_ms = resync_res == ESP_OK
                                       ? APP_NTP_RESYNC_INTERVAL_MS
                                       : APP_NTP_WIFI_WAIT_MS;
            proxima_resincronizacao_us = esp_timer_get_time() + (int64_t)proximo_ms * 1000;
        }

        esp_err_t dht_res = sensores_ambiente_ler(&umidade, &temperatura_dht);
        if (dht_res == ESP_OK && (!isfinite(umidade) || !isfinite(temperatura_dht))) {
            dht_res = ESP_FAIL;
        }
        if (dht_res != ESP_OK) {
            umidade = 50.0f;
            temperatura_dht = 25.0f;
            ESP_LOGW(TAG, "DHT indisponivel (%s): fallback simulado de 50%% umidade / 25 C",
                     esp_err_to_name(dht_res));
        }

        float temperatura_interna = 0.0f;
        esp_err_t temp_interna_res = sensores_ambiente_ler_temperatura_interna(&temperatura_interna);
        bool temp_interna_valida = temp_interna_res == ESP_OK && isfinite(temperatura_interna);
        float temperatura = temp_interna_valida ? temperatura_interna : runtime->medicoes.temperatura;
        if (!temp_interna_valida) {
            ESP_LOGW(TAG, "Sensor interno de temperatura indisponivel (%s): mantendo ultima leitura valida",
                     esp_err_to_name(temp_interna_res));
        }

        esp_err_t rtc_res = tempo_ler_sistema(&rtc_tempo);
        if (rtc_res != ESP_OK) {
            rtc_res = tempo_ler_ds3231(&runtime->handles, &rtc_tempo);
            ts_source = (rtc_res == ESP_OK) ? "DS3231" : "NONE";
        }

        if (runtime_lock(runtime)) {
            runtime->medicoes.dht_res = dht_res;
            runtime->medicoes.rtc_res = rtc_res;
            runtime->medicoes.ts_source = ts_source;

            runtime->medicoes.umidade = umidade;
            runtime->medicoes.temperatura = temperatura;
            runtime->medicoes.temperatura_ambiente = temperatura_dht;

            if (rtc_res == ESP_OK) {
                runtime->medicoes.rtc_tempo = rtc_tempo;
            }

            runtime_unlock(runtime);
        }

        esp_task_wdt_reset();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(APP_TASK_PERIOD_AMBIENTE_MS));
    }
}

/* Deliberately not subscribed to the task watchdog: http_client_app's
 * timeout_ms (5000) already sits right at the global 5 s CONFIG_ESP_TASK_WDT_TIMEOUT_S,
 * so a slow-but-legitimate HTTPS round trip or connection re-establishment
 * could trip a false panic-reboot. Lowering the
 * HTTP timeout to make room would weaken the retry/backoff behavior this
 * task already relies on for genuine network flakiness. */
static void task_envio_telemetria(void *arg)
{
    app_runtime_t *runtime = (app_runtime_t *)arg;
    uint32_t backoff_ms = APP_HTTP_RETRY_INITIAL_MS;

    while (1) {
        uint32_t espera_ms = APP_TASK_PERIOD_ENVIO_MS;
        app_medicoes_t medicoes = {0};
        app_shared_data_t shared = {0};

        esp_err_t resultado_validacao = ESP_FAIL;
        int status_validacao = -1;

        if (runtime_snapshot(runtime, &medicoes, &shared) &&
            wifi_app_esta_conectado(&shared) &&
            medicoes.zmpt_res == ESP_OK && medicoes.sct_res == ESP_OK) {
            char *payload = telemetria_montar_json(&medicoes, &shared);
            if (payload != NULL) {
                int status_code = -1;
                esp_err_t envio_res = http_client_app_enviar_telemetria(payload, &status_code);
                resultado_validacao = envio_res;
                status_validacao = status_code;
                bool envio_ok = envio_res == ESP_OK && status_code >= 200 && status_code < 300;
                if (envio_ok) {
                    backoff_ms = APP_HTTP_RETRY_INITIAL_MS;
                } else {
                    /* Includes ESP_ERR_HTTP_CONNECT and other transport/server
                     * failures. No retry loop inside the HTTP client. */
                    espera_ms = (status_code == 401 || status_code == 403 || status_code == 429)
                        ? 60000U : backoff_ms;
                    backoff_ms = APP_HTTP_RETRY_MAX_MS;
                    ESP_LOGW(TAG,
                             "POST falhou (err=%s status=%d); nova tentativa em %lu ms",
                             esp_err_to_name(envio_res), status_code, (unsigned long)espera_ms);
                }

                if (runtime_lock(runtime)) {
                    runtime->shared.ultimo_envio_res = envio_res;
                    runtime->shared.ultimo_http_status = status_code;

                    if (envio_ok) {
                        runtime->shared.total_envios_ok++;
                    } else {
                        runtime->shared.total_envios_erro++;
                    }

                    runtime_unlock(runtime);
                }

            }
        }

        ota_app_registrar_telemetria(resultado_validacao, status_validacao);
        /* Count from completion, never catch up missed 500 ms send deadlines
         * after a slow connection attempt. Only the sender task sleeps here. */
        vTaskDelay(pdMS_TO_TICKS(espera_ms));
    }
}

static void task_persistencia_energia(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    while (true) {
        esp_err_t res = shared_data_persistir_energia();
        if (res != ESP_OK) {
            ESP_LOGW(TAG, "Falha ao persistir energia: %s", esp_err_to_name(res));
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void task_display(void *arg)
{
    app_runtime_t *runtime = (app_runtime_t *)arg;
    enum tela_atual tela = TELA_ELETRICA_BASICA;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    while (1) {
        app_medicoes_t medicoes = {0};
        app_shared_data_t shared = {0};

        if (runtime_snapshot(runtime, &medicoes, &shared)) {
            display_mostrar_debug(&tela,
                                  &shared,
                                  &medicoes,
                                  runtime->handles.oled_handle);

        }

        esp_task_wdt_reset();
        /* Tempo integral de leitura, sem compensar atrasos com trocas rapidas. */
        vTaskDelay(pdMS_TO_TICKS(APP_TASK_PERIOD_DISPLAY_MS));
    }
}

esp_err_t app_tasks_start(app_runtime_t *runtime)
{
    if (runtime == NULL || runtime->mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (runtime->tasks_started || xSharedDataMutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* A partial startup must not create duplicate ADC owners on retry. */
    runtime->tasks_started = true;

    if (xTaskCreatePinnedToCore(task_persistencia_energia, "task_energia_nvs",
                               3072, NULL, 1, NULL, APP_TASK_CORE_REDE) != pdPASS) {
        runtime->tasks_started = false;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(task_aquisicao_eletrica,
                     "task_eletrica",
                     4096,
                     runtime,
                     5,
                     NULL,
                     APP_TASK_CORE_ELETRICA);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    ok = xTaskCreatePinnedToCore(task_aquisicao_ambiente_tempo,
                     "task_ambiente",
                     4096,
                     runtime,
                     4,
                     NULL,
                     APP_TASK_CORE_AMBIENTE);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    ok = xTaskCreatePinnedToCore(task_envio_telemetria,
                     "task_envio",
                     6144,
                     runtime,
                     3,
                     NULL,
                     APP_TASK_CORE_REDE);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    ok = xTaskCreatePinnedToCore(task_display,
                     "task_display",
                     4096,
                     runtime,
                     2,
                     NULL,
                     APP_TASK_CORE_REDE);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }

    ok = xTaskCreatePinnedToCore(ota_task, "ota_task", 8192, runtime,
                                     2, NULL, APP_TASK_CORE_REDE);
    if (ok != pdPASS) { return ESP_FAIL; }

    ESP_LOGI(TAG,
             "Tasks iniciadas (eletrica=%dms ambiente=%dms envio=%dms display=%dms)",
             APP_TASK_PERIOD_ELETRICA_MS,
             APP_TASK_PERIOD_AMBIENTE_MS,
             APP_TASK_PERIOD_ENVIO_MS,
             APP_TASK_PERIOD_DISPLAY_MS);

    return ESP_OK;
}
