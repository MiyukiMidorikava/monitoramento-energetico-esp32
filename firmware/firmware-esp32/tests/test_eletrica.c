#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_platform.h"

static uint32_t xEventGroupWaitBits(EventGroupHandle_t group, uint32_t bits,
                                   BaseType_t clear, BaseType_t all, TickType_t timeout)
{ (void)group; (void)clear; (void)all; (void)timeout; return bits; }

/* Include the real task to exercise its post-filter clamp and integration. */
#include "../main/sensores_eletricos.c"
#include "../main/shared_data.c"
#include "../main/tasks_app.c"
#include "../main/telemetria.c"

static esp_err_t wifi_result = ESP_OK;
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record)
{ record->rssi = -57; return wifi_result; }

static uint32_t fake_heap_free_bytes = 123456;
esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_TASK_WDT; }
const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t app = {.app_elf_sha256 = {[0] = 0xab, [31] = 0xcd}};
    return &app;
}
uint32_t esp_get_free_heap_size(void) { return fake_heap_free_bytes; }
static UBaseType_t fake_stack_min_bytes = 4096;
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{ (void)task; return fake_stack_min_bytes; }

static size_t allocation_count, fail_allocation, live_allocations;
static void *fault_malloc(size_t size)
{
    if (++allocation_count == fail_allocation) { return NULL; }
    void *ptr = malloc(size);
    if (ptr != NULL) { ++live_allocations; }
    return ptr;
}
static void fault_free(void *ptr)
{
    if (ptr != NULL) { assert(live_allocations > 0); --live_allocations; }
    free(ptr);
}
static void test_json_allocation_failures(void)
{
    allocation_count = fail_allocation = 0;
    app_medicoes_t measures = {0};
    app_shared_data_t shared = {0};
    cJSON_Hooks hooks = {.malloc_fn = fault_malloc, .free_fn = fault_free};
    cJSON_InitHooks(&hooks);
    char *payload = telemetria_montar_json(&measures, &shared);
    assert(payload != NULL);
    size_t total = allocation_count;
    assert(live_allocations == 0);
    for (size_t k = 1; k <= total; ++k) {
        allocation_count = 0;
        fail_allocation = k;
        payload = telemetria_montar_json(&measures, &shared);
        assert(payload == NULL);
        assert(live_allocations == 0);
    }
    cJSON_InitHooks(NULL);
    puts("PASS: cJSON allocation failures return NULL without leaks");
}

static bool testing_ambient;
static esp_err_t ambient_result;
static int64_t now_us;
static uint32_t adc_position;
static int voltage_counts = 200, current_counts = 100;
static int voltage_bias = 2200, current_bias = 1900;
static int phase_quarters;
static bool sine_wave;
static bool half_count_noise;
static bool rf_impulse;
static bool rf_voltage_impulse, rf_heavy;
static bool in_send;
static int send_attempts;
static TickType_t send_delays[4];
static int64_t send_finished_us[4], send_started_us[4];
static bool adc_empty, adc_fail, overflow, corrupt, running;
static adc_continuous_evt_cbs_t callbacks;
static jmp_buf task_exit;
static app_runtime_t test_runtime;
static app_medicoes_t snapshots[16];
static int64_t snapshot_times[16];
static int cycles, target_cycles, scenario;
static double boot_energy;
static bool in_snapshot;
static int electrical_core = -1, ambient_core = -1, current_core = 1, adc_irq_core = -1;
static unsigned adc_init_count;
static void *callback_owner;
static uint32_t notifications;
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)42; }
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *wake)
{ assert(task == (void *)42); ++notifications; *wake = pdTRUE; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout)
{
    assert(clear == pdTRUE && timeout > 0);
    /* Model a notification delivered by the DMA core while the other core
     * cannot service a tick. No vTaskDelay or runtime mutex is required. */
    if (!adc_empty) { callbacks.on_conv_done((void *)1, NULL, callback_owner); }
    now_us += 10000;
    uint32_t result = notifications;
    notifications = 0;
    return result;
}

static bool mutex_timeout;
static bool measurement_locked;
static double flash_energy = 12.5;
static double staged_energy;
static int commits;
static bool commit_fails;
static esp_err_t read_result = ESP_OK;
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)
{ assert(strcmp(ns, "energia") == 0 && mode == NVS_READWRITE); *h = 1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *value, size_t *size)
{ (void)h; assert(strcmp(key, "energia_kwh") == 0 && *size == sizeof(double));
  if (read_result != ESP_OK) { return read_result; }
  memcpy(value, &flash_energy, *size); return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t size)
{ (void)h; (void)key; assert(!measurement_locked && size == sizeof(double)); memcpy(&staged_energy, value, size); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h)
{ (void)h; assert(!measurement_locked); if (commit_fails) { return ESP_FAIL; } flash_energy = staged_energy; ++commits; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
void ota_app_registrar_telemetria(esp_err_t res, int status) { (void)res; (void)status; }

static void test_nvs_boot_variants(void)
{
    const double invalid[] = {NAN, INFINITY, -1.0, 0.0};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        /* Model reset of RAM; the simulated flash survives. */
        xSharedDataMutex = NULL;
        ultima_medicao = (medicoes_eletricas_t){.resultado = ESP_ERR_INVALID_STATE};
        flash_energy = invalid[i];
        assert(shared_data_init() == ESP_OK);
        medicoes_eletricas_t value;
        assert(shared_data_get_medicoes(&value) == ESP_OK && value.energia_kwh == 0);
    }
    xSharedDataMutex = NULL;
    read_result = ESP_FAIL;
    assert(shared_data_init() == ESP_FAIL && xSharedDataMutex == NULL);
    read_result = ESP_ERR_NVS_NOT_FOUND;
    assert(shared_data_init() == ESP_OK);
    read_result = ESP_OK;
    medicoes_eletricas_t value = {.energia_kwh = 0.005};
    assert(shared_data_set_medicoes(&value) == ESP_OK);
    int before = commits;
    assert(shared_data_persistir_energia() == ESP_OK && commits == before + 1);
    assert(flash_energy == 0.005); /* Exact 5 Wh boundary without waiting 5 min. */
    xSharedDataMutex = NULL;
    ultima_medicao = (medicoes_eletricas_t){.resultado = ESP_ERR_INVALID_STATE};
    assert(shared_data_init() == ESP_OK);
    assert(shared_data_get_medicoes(&value) == ESP_OK && value.energia_kwh == 0.005);
    puts("PASS: NVS empty/invalid/read-error boots, exact 5 Wh threshold, reboot after commit");
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)2; }
void ota_task(void *arg) { (void)arg; }
/* Each simulated "task" function here is re-entered from the top on every
 * call (via setjmp/longjmp, not a persistent FreeRTOS task loop), so these
 * stubs cannot assert a real subscribe/unsubscribe state machine -- they
 * only need to let tasks_app.c/ota_app.c link and run. */
esp_err_t esp_task_wdt_add(TaskHandle_t task) { (void)task; return ESP_OK; }
esp_err_t esp_task_wdt_delete(TaskHandle_t task) { (void)task; return ESP_OK; }
esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }

const char *esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "ESP_FAIL"; }
int64_t esp_timer_get_time(void) { return now_us; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(now_us / 1000); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{ if (s == xSharedDataMutex) { assert(ticks == 50); measurement_locked = !mutex_timeout; return !mutex_timeout; }
  assert(in_snapshot || in_send || testing_ambient); return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{ if (s == xSharedDataMutex) { measurement_locked = false; } return pdTRUE; }
void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0);
    if (in_send) {
        send_delays[send_attempts - 1] = ticks;
        if (send_attempts == 4) { longjmp(task_exit, 1); }
    }
    now_us += (int64_t)ticks * 1000;
    if (in_send) {
        /* Model the independent ADC owner publishing while HTTP sleeps. */
        ultima_medicao.t_leitura_us = now_us;
        ultima_medicao.resultado = ESP_OK;
    }
}
void vTaskDelayUntil(TickType_t *last, TickType_t ticks)
{
    if (testing_ambient) { longjmp(task_exit, 1); }
    if (in_send) {
        *last += ticks;
        TickType_t remaining = *last > xTaskGetTickCount() ? *last - xTaskGetTickCount() : 0;
        send_delays[send_attempts - 1] = remaining;
        if (send_attempts == 4) { longjmp(task_exit, 1); }
        now_us += (int64_t)remaining * 1000;
        return;
    }
    app_shared_data_t shared;
    in_snapshot = true;
    assert(runtime_snapshot(&test_runtime, &snapshots[cycles], &shared));
    in_snapshot = false;
    snapshot_times[cycles] = now_us;
    if (++cycles == target_cycles) { longjmp(task_exit, 1); }
    if (scenario == 1) { current_counts = 0; }
    if (scenario == 2) { adc_fail = cycles == 1; }
    if (scenario == 3) {
        *last += 3600000;
        now_us = (int64_t)*last * 1000;
    } else {
        *last += ticks;
        now_us = (int64_t)*last * 1000 + cycles * 37000; /* Deliberate scheduling jitter. */
    }
}
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, unsigned priority, void *out)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)out; return pdPASS; }
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg, unsigned priority, void *out, BaseType_t core)
{
    if (fn == task_aquisicao_eletrica) { electrical_core = core; assert(priority == 5); }
    else { assert(core == 0); }
    if (fn == task_aquisicao_ambiente_tempo) { ambient_core = core; }
    return xTaskCreate(fn, name, stack, arg, priority, out);
}
esp_err_t sensores_ambiente_ler(float *a, float *b) { *a = 61.5f; *b = 28.25f; return ambient_result; }
esp_err_t sensores_ambiente_ler_temperatura_interna(float *temperature)
{ *temperature = 28.25f; return ESP_OK; }
bool wifi_app_esta_conectado(const app_shared_data_t *shared)
{ (void)shared; return wifi_result == ESP_OK; }
esp_err_t tempo_sincronizar_boot(app_handles_t *handles, app_shared_data_t *shared)
{ (void)handles; shared->boot_sync_source = "NONE"; return ESP_FAIL; }
esp_err_t tempo_resincronizar_ntp(app_handles_t *handles)
{ (void)handles; return ESP_FAIL; }
esp_err_t tempo_ler_sistema(struct tm *t) { (void)t; return ESP_FAIL; }
esp_err_t tempo_ler_ds3231(const app_handles_t *h, struct tm *t) { (void)h; (void)t; return ESP_FAIL; }
esp_err_t http_client_app_enviar_telemetria(const char *p, int *s)
{
    assert(in_send && p != NULL && send_attempts < 4);
    send_started_us[send_attempts] = now_us;
    now_us += 2700000; /* Connection work itself exceeds the normal send period. */
    send_finished_us[send_attempts] = now_us;
    ++send_attempts;
    *s = send_attempts == 3 ? 200 : -1;
    return send_attempts == 3 ? ESP_OK : ESP_ERR_HTTP_CONNECT;
}
void display_mostrar_debug(enum tela_atual *m, const app_shared_data_t *s, const app_medicoes_t *v, ssd1306_handle_t h)
{ (void)m; (void)s; (void)v; (void)h; }

esp_err_t adc_continuous_new_handle(const adc_continuous_handle_cfg_t *c, adc_continuous_handle_t *h)
{ assert(c->max_store_buf_size >= 4800); *h = (void *)1; adc_irq_core = current_core; ++adc_init_count; return ESP_OK; }
esp_err_t adc_continuous_config(adc_continuous_handle_t h, const adc_continuous_config_t *c)
{ (void)h; assert(c->pattern_num == 2); return ESP_OK; }
esp_err_t adc_continuous_register_event_callbacks(adc_continuous_handle_t h, const adc_continuous_evt_cbs_t *c, void *u)
{ (void)h; callbacks = *c; callback_owner = u; return ESP_OK; }
esp_err_t adc_continuous_start(adc_continuous_handle_t h)
{ (void)h; assert(!running); running = true; adc_position = 0; return ESP_OK; }
esp_err_t adc_continuous_stop(adc_continuous_handle_t h)
{ (void)h; assert(running); running = false; return ESP_OK; }
esp_err_t adc_continuous_flush_pool(adc_continuous_handle_t h)
{ (void)h; assert(!running); return ESP_OK; }
esp_err_t adc_continuous_deinit(adc_continuous_handle_t h) { (void)h; return ESP_OK; }
esp_err_t adc_continuous_read(adc_continuous_handle_t h, uint8_t *buf, uint32_t capacity, uint32_t *length, uint32_t timeout)
{
    (void)h;
    assert(running && timeout == 0); /* No waiting for ADC data. */
    *length = 0;
    if (adc_fail) { return ESP_FAIL; }
    if (adc_empty) { return ESP_ERR_TIMEOUT; }
    if (overflow) { callbacks.on_pool_ovf(h, NULL, callback_owner); }
    /* Odd chunk sizes force the production parser to preserve pair boundaries. */
    uint32_t count = capacity / sizeof(adc_digi_output_data_t);
    if (count > 119) { count = 119; }
    for (uint32_t k = 0; k < count; ++k, ++adc_position) {
        unsigned pair = adc_position / 2;
        bool is_current = adc_position % 2;
        int sign = ((pair + (is_current ? phase_quarters : 0)) % 4 < 2) ? 1 : -1;
        int ac = sign * (is_current ? current_counts : voltage_counts);
        if (is_current && half_count_noise && pair % 2 == 0) { ac -= sign; }
        if (sine_wave) {
            double angle = pair * 0.06283185307179586 + (is_current ? 1.0471975511965977 : 0);
            ac = (int)lround(sin(angle) * (is_current ? current_counts : voltage_counts));
        }
        if (rf_impulse && pair == 51 && is_current) { ac += 2000; }
        if (rf_voltage_impulse && pair == 51 && !is_current) { ac += 1000; }
        if (rf_heavy && pair % 4 == 3 && is_current) { ac += 2000; }
        /* Pack the physical wire layout independently of the struct under test.
         * A set reserved bit must never shift channel decoding or enter the RMS. */
        uint32_t channel = corrupt ? 7 : (is_current ? SCT_ADC_CHANNEL : ZMPT_ADC_CHANNEL);
        uint32_t word = (uint32_t)((is_current ? current_bias : voltage_bias) + ac)
                      | (1u << 12) | (channel << 13) | ((uint32_t)ADC_UNIT_1 << 17);
        memcpy(buf + k * sizeof(word), &word, sizeof(word));
    }
    *length = count * sizeof(adc_digi_output_data_t);
    return ESP_OK;
}

static void near_at(double actual, double expected, double tolerance, int line)
{
    if (!isfinite(actual) || fabs(actual - expected) > tolerance) {
        fprintf(stderr, "line %d: actual %.12g expected %.12g tolerance %.12g\n", line, actual, expected, tolerance);
        assert(false);
    }
}

#define near(actual, expected, tolerance) near_at(actual, expected, tolerance, __LINE__)

static esp_err_t read_window(sensores_eletricos_grandezas_t *out)
{
    esp_err_t result;
    int attempts = 0;
    do {
        result = sensores_eletricos_ler_grandezas((void *)1, out);
        now_us += 10000;
        assert(++attempts < 100);
    } while (result == ESP_ERR_NOT_FINISHED);
    return result;
}

static void reset_filters(void)
{
    /* Flush using the public filters, without relying on private buffer layout. */
    for (int k = 0; k < ZMPT_FILTER_WINDOW; ++k) {
        sensores_eletricos_filtrar_tensao(0);
        sensores_eletricos_filtrar_corrente(0);
        sensores_eletricos_filtrar_potencia(0);
    }
}

static void run_task(int count, int mode)
{
    medicoes_eletricas_t initial = {.energia_kwh = boot_energy};
    assert(shared_data_set_medicoes(&initial) == ESP_OK);
    memset(&test_runtime, 0, sizeof(test_runtime));
    test_runtime.mutex = (void *)1;

    current_core = electrical_core;
    unsigned init_count_before = adc_init_count;
    cycles = 0;
    target_cycles = count;
    scenario = mode;
    adc_fail = false;
    if (setjmp(task_exit) == 0) { task_aquisicao_eletrica(&test_runtime); }
    assert(adc_init_count == init_count_before + 1);
    assert(adc_irq_core == electrical_core && adc_irq_core != ambient_core);

}

static void test_ambient_pipeline(void)
{
    test_runtime.mutex = (void *)1;
    testing_ambient = true;
    const esp_err_t results[] = {ESP_FAIL, ESP_OK, ESP_ERR_TIMEOUT, ESP_OK};
    for (unsigned i = 0; i < 4; ++i) {
        ambient_result = results[i];
        if (setjmp(task_exit) == 0) { task_aquisicao_ambiente_tempo(&test_runtime); }
        near(test_runtime.medicoes.temperatura, 28.25, 0);
        near(test_runtime.medicoes.temperatura_ambiente, results[i] == ESP_OK ? 28.25 : 25, 0);
        near(test_runtime.medicoes.umidade, results[i] == ESP_OK ? 61.5 : 50, 0);
        assert(test_runtime.medicoes.dht_res == results[i]);
        char *payload = telemetria_montar_json(&test_runtime.medicoes, &test_runtime.shared);
        assert(payload != NULL);
        cJSON *json = cJSON_Parse(payload);
        cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "dados");
        cJSON *temp = cJSON_GetObjectItemCaseSensitive(data, "temperatura_c");
        cJSON *temp_ambiente = cJSON_GetObjectItemCaseSensitive(data, "temperatura_ambiente_c");
        cJSON *humidity = cJSON_GetObjectItemCaseSensitive(data, "umidade_pct");
        assert(cJSON_IsNumber(temp) && cJSON_IsNumber(temp_ambiente) && cJSON_IsNumber(humidity));
        near(temp->valuedouble, 28.25, 0.00002);
        near(temp_ambiente->valuedouble, results[i] == ESP_OK ? 28.25 : 25, 0.00002);
        near(humidity->valuedouble, results[i] == ESP_OK ? 61.5 : 50, 0.00002);
        cJSON *status = cJSON_GetObjectItemCaseSensitive(json, "status");
        cJSON *valid = cJSON_GetObjectItemCaseSensitive(status, "dht_valido");
        assert(cJSON_IsNumber(valid));
        near(valid->valuedouble, results[i] == ESP_OK ? 1 : 0, 0);
        cJSON_Delete(json);
    }
    testing_ambient = false;
    memset(&test_runtime, 0, sizeof(test_runtime));
    puts("PASS: ambient readings, fallback, recovery and JSON");
}

static void test_complete_telemetry(void)
{
    app_medicoes_t m = {.tensao_rms = 127, .corrente_rms = 2,
        .potencia_w = 203.2f, .potencia_va = 254,
        .potencia_reativa_var = 152.4f, .fator_potencia = 0.8f,
        .energia_kwh = 0.000123456789,
        .temperatura = 42.5f, .temperatura_ambiente = 24.3f, .umidade = 61.5f,
        .corrente_pre_correcao_a = 2.334f};
    app_shared_data_t shared = {0};
    char *payload = telemetria_montar_json(&m, &shared);
    assert(payload != NULL);
    cJSON *json = cJSON_Parse(payload);
    cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "dados");
    const char *keys[] = {"tensao_rms_v", "corrente_rms_a", "potencia_ativa_w",
        "fator_potencia", "energia_kwh", "potencia_va", "potencia_reativa_var",
        "temperatura_c", "temperatura_ambiente_c", "umidade_pct"};
    const double expected[] = {127, 2, 203.2, 0.8, 0.000123456789, 254, 152.4, 42.5, 24.3, 61.5};
    for (unsigned k = 0; k < 10; ++k) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(data, keys[k]);
        assert(cJSON_IsNumber(value));
        near(value->valuedouble, expected[k], k == 4 ? 1e-12 : 0.00002);
    }
    assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(json, "uptime_ms")));
    cJSON *diagnostics = cJSON_GetObjectItemCaseSensitive(json, "status");
    cJSON *reason = cJSON_GetObjectItemCaseSensitive(diagnostics, "reset_reason");
    assert(cJSON_IsNumber(reason));
    near(reason->valuedouble, 6, 0);
    cJSON *valid = cJSON_GetObjectItemCaseSensitive(diagnostics, "dht_valido");
    assert(cJSON_IsNumber(valid));
    near(valid->valuedouble, 1, 0);
    cJSON *raw = cJSON_GetObjectItemCaseSensitive(diagnostics, "corrente_pre_correcao_a");
    assert(cJSON_IsNumber(raw));
    near(raw->valuedouble, 2.334, 0.00001);
    cJSON *identity = cJSON_GetObjectItemCaseSensitive(json, "firmware_sha256");
    assert(cJSON_IsString(identity));
    assert(strcmp(identity->valuestring,
        "ab000000000000000000000000000000000000000000000000000000000000cd") == 0);
    cJSON_Delete(json);
    wifi_result = ESP_FAIL;
    payload = telemetria_montar_json(&m, &shared);
    assert(payload != NULL);
    json = cJSON_Parse(payload);
    cJSON *status = cJSON_GetObjectItemCaseSensitive(json, "status");
    /* wifi_rssi is skipped without an AP, but heap/stack diagnostics are
     * independent of Wi-Fi and must still be present. */
    assert(cJSON_IsObject(status) && cJSON_GetArraySize(status) == 5);
    near(cJSON_GetObjectItemCaseSensitive(status, "heap_livre_bytes")->valuedouble, fake_heap_free_bytes, 0);
    near(cJSON_GetObjectItemCaseSensitive(status, "stack_min_bytes")->valuedouble, fake_stack_min_bytes, 0);
    assert(cJSON_GetObjectItemCaseSensitive(status, "wifi_rssi") == NULL);
    cJSON_Delete(json);
    wifi_result = ESP_OK;
    /* Full precision/scientific notation must fit, including diagnostics. */
    m.tensao_rms = m.corrente_rms = m.potencia_w = m.potencia_va = 3.402823e38f;
    m.potencia_reativa_var = m.fator_potencia = m.temperatura = -3.402823e38f;
    m.temperatura_ambiente = m.umidade = m.corrente_pre_correcao_a = 1.234567e-30f;
    m.energia_kwh = 1.7976931348623157e308;
    payload = telemetria_montar_json(&m, &shared);
    assert(payload != NULL);
    json = cJSON_Parse(payload);
    assert(json != NULL);
    cJSON_Delete(json);
    test_json_allocation_failures();
    puts("PASS: complete electrical JSON and energy precision");
}

static void test_telemetry_rejects_invalid_measurements(void)
{
    app_medicoes_t m = {0};
    app_shared_data_t shared = {0};
    float *values[] = {&m.tensao_rms, &m.corrente_rms, &m.potencia_w, &m.fator_potencia};
    const float invalid[] = {NAN, INFINITY, -INFINITY};
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned j = 0; j < 3; ++j) {
            *values[i] = invalid[j];
            assert(telemetria_montar_json(&m, &shared) == NULL);
        }
        *values[i] = 0;
    }
    for (unsigned j = 0; j < 3; ++j) {
        m.energia_kwh = invalid[j];
        assert(telemetria_montar_json(&m, &shared) == NULL);
    }
    m.energia_kwh = 0;
    m.corrente_pre_correcao_a = NAN;
    assert(telemetria_montar_json(&m, &shared) == NULL);
    m.corrente_pre_correcao_a = -1;
    assert(telemetria_montar_json(&m, &shared) == NULL);
    m.corrente_pre_correcao_a = 0;
    m.zmpt_res = ESP_ERR_INVALID_STATE;
    assert(telemetria_montar_json(&m, &shared) == NULL);
    m.zmpt_res = ESP_OK;
    m.sct_res = ESP_FAIL;
    assert(telemetria_montar_json(&m, &shared) == NULL);
    m.sct_res = ESP_OK;
    int64_t saved = now_us;
    now_us = INT64_C(9007199254740991000);
    char *payload = telemetria_montar_json(&m, &shared);
    assert(payload != NULL);
    assert(strstr(payload, "\"uptime_ms\":9007199254740991,") != NULL);
    now_us = saved;
    puts("PASS: invalid ADC/NaN/Inf rejected and uptime serialized as exact decimal integer");
}

static void test_measurement_freshness(void)
{
    assert(shared_data_init() == ESP_OK);
    test_runtime.mutex = (void *)1;
    int64_t saved = now_us;
    now_us = 10000000;
    const int64_t ages[] = {0, 1500000, 1500001, -1, 10000000};
    for (unsigned i = 0; i < sizeof ages / sizeof ages[0]; ++i) {
        medicoes_eletricas_t measurement = {
            .resultado = ESP_OK, .t_leitura_us = now_us - ages[i],
        };
        assert(shared_data_set_medicoes(&measurement) == ESP_OK);
        app_medicoes_t m;
        app_shared_data_t shared;
        in_snapshot = true;
        assert(runtime_snapshot(&test_runtime, &m, &shared));
        in_snapshot = false;
        char *payload = telemetria_montar_json(&m, &shared);
        assert((payload != NULL) == (i < 2));
    }
    now_us = saved;
    puts("PASS: startup, future and stale ADC snapshots cannot be serialized");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* A 5 W load and a low-PF load are both legitimate consumption. */
    float small_i = 0.04f, small_p = 5.0f, small_s = 5.08f, small_fp = small_p / small_s;
    sensores_eletricos_aplicar_deadband(&small_i, &small_p, &small_s, &small_fp);
    sensores_eletricos_aplicar_trava_potencia_real(&small_i, &small_p, &small_s, &small_fp);
    near(small_p, 5.0, 0);
    small_i = 1.0f; small_p = 10.0f; small_s = 127.0f; small_fp = small_p / small_s;
    sensores_eletricos_aplicar_trava_potencia_real(&small_i, &small_p, &small_s, &small_fp);
    near(small_p, 10.0, 0);
    small_p = -20.0f; small_fp = small_p / small_s;
    sensores_eletricos_aplicar_trava_potencia_real(&small_i, &small_p, &small_s, &small_fp);
    near(small_p, 0, 0);
    near(small_fp, 0, 0);
    near(small_i, 1.0, 0); /* A phase error must not erase measured RMS. */
    near(small_s, 127.0, 0);
    test_ambient_pipeline();
    test_complete_telemetry();
    test_telemetry_rejects_invalid_measurements();
    if (getenv("TEST_TELEMETRIA_ONLY") != NULL) { test_measurement_freshness(); return 0; }

    medicoes_eletricas_t original = {.tensao_rms = 127, .energia_kwh = 123.456};
    medicoes_eletricas_t dest = {0}, replacement = {.tensao_rms = 220};
    assert(shared_data_get_medicoes(&dest) == ESP_ERR_INVALID_STATE);
    assert(shared_data_init() == ESP_OK);
    assert(shared_data_get_medicoes(&dest) == ESP_OK);
    assert(dest.energia_kwh == 12.5);
    dest.energia_kwh = 12.504;
    assert(shared_data_set_medicoes(&dest) == ESP_OK);
    now_us = 299999999;
    assert(shared_data_persistir_energia() == ESP_OK && commits == 0);
    now_us = 300000000;
    assert(shared_data_persistir_energia() == ESP_OK && commits == 1);
    assert(flash_energy == 12.504);
    dest.energia_kwh = 12.510;
    assert(shared_data_set_medicoes(&dest) == ESP_OK);
    ++now_us;
    commit_fails = true;
    assert(shared_data_persistir_energia() == ESP_FAIL && commits == 1);
    assert(flash_energy == 12.504);
    commit_fails = false;
    assert(shared_data_persistir_energia() == ESP_OK && commits == 1);
    now_us += 10000000;
    assert(shared_data_persistir_energia() == ESP_OK && commits == 2);
    assert(flash_energy == 12.510);
    now_us += 300000000;
    assert(shared_data_persistir_energia() == ESP_OK && commits == 2);
    puts("PASS: NVS restored energy, 5-minute/delta writes, commit failure/retry, no idle writes");
    test_nvs_boot_variants();
    assert(shared_data_init() == ESP_OK);
    assert(shared_data_set_medicoes(NULL) == ESP_ERR_INVALID_ARG);
    assert(shared_data_get_medicoes(NULL) == ESP_ERR_INVALID_ARG);
    assert(shared_data_set_medicoes(&original) == ESP_OK);
    mutex_timeout = true;
    assert(shared_data_set_medicoes(&replacement) == ESP_ERR_TIMEOUT);
    assert(shared_data_get_medicoes(&dest) == ESP_ERR_TIMEOUT);
    assert(dest.tensao_rms == 0);
    mutex_timeout = false;
    assert(shared_data_get_medicoes(&dest) == ESP_OK);
    assert(dest.tensao_rms == 127 && dest.energia_kwh == original.energia_kwh);
    app_runtime_t startup = {.mutex = (void *)1};
    assert(app_tasks_start(&startup) == ESP_OK);
    assert(electrical_core == 1 && ambient_core == 0);
    assert(app_tasks_start(&startup) == ESP_ERR_INVALID_STATE);
    if (getenv("TEST_PERSISTENCIA_ONLY") != NULL) {
        reset_filters();
        boot_energy = flash_energy;
        run_task(4, 2);
        near(snapshots[0].energia_kwh, boot_energy, 0);
        near(snapshots[1].energia_kwh, boot_energy, 0);
        near(snapshots[2].energia_kwh, boot_energy, 0);
        assert(snapshots[3].energia_kwh > boot_energy);
        puts("PASS: restored accumulator survives ADC failure and resumes integration");
        return 0;
    }
    adc_continuous_handle_t handle;
    assert(sensores_eletricos_inicializar(&handle) == ESP_OK);
    sensores_eletricos_grandezas_t reading = {0};
    assert(sensores_eletricos_ler_grandezas(NULL, &reading) == ESP_ERR_INVALID_STATE);
    assert(sensores_eletricos_ler_grandezas(handle, NULL) == ESP_ERR_INVALID_ARG);

    assert(read_window(&reading) == ESP_OK);
    /* +/-200 and +/-100 ADC counts with different DC offsets: analytic values.
     * Fixtures use the 2026-09-22 bench gains (634.667 V/V, 77.549 A/V)
     * and 1.2020 A baseline; preserve tolerances when recalibrating. */
    near(reading.tensao_rms, 102.290649414, 0.0001);
    near(reading.corrente_rms, 6.132684708, 0.0002);
    near(reading.potencia_w, 639.252136230, 0.001);
    near(reading.offset_tensao_counts, 2200, 0);
    near(reading.offset_corrente_counts, 1900, 0);
    voltage_bias = 1500;
    current_bias = 2700;
    assert(read_window(&reading) == ESP_OK);
    near(reading.tensao_rms, 102.290649414, 0.0001);
    near(reading.corrente_rms, 6.132684708, 0.0002);
    near(reading.potencia_w, 639.252136230, 0.001);
    near(reading.offset_tensao_counts, 1500, 0);
    near(reading.offset_corrente_counts, 2700, 0);
    voltage_bias = 2200;
    current_bias = 1900;
    phase_quarters = 1;
    assert(read_window(&reading) == ESP_OK);
    near(reading.potencia_w, 0, 0.0001);
    phase_quarters = 2;
    assert(read_window(&reading) == ESP_OK);
    near(reading.potencia_w, -639.252136230, 0.001);
    phase_quarters = 0;
    sine_wave = true;
    voltage_counts = 1000;
    current_counts = 500;
    assert(read_window(&reading) == ESP_OK);
    /* Six cycles, 60 degree displacement: P = Vpk * Ipk * cos(60) / 2. */
    near(reading.potencia_w, 3995.32582235, 5.0); /* ADC quantization tolerance. */
    sine_wave = false;
    voltage_counts = 200;
    current_counts = 19; /* Below the measured 1.2020 A baseline. */
    assert(read_window(&reading) == ESP_OK);
    near(reading.corrente_pre_correcao_a, 19.0 * 3.3 / 4095 * 77.549, 0.00001);
    near(reading.corrente_rms, 0, 0);
    near(reading.potencia_w, 0, 0); /* Zero current must never retain active power. */

    reading.potencia_w = 123;
    adc_empty = true;
    assert(read_window(&reading) == ESP_ERR_TIMEOUT);
    near(reading.potencia_w, 123, 0); /* Failed window must not overwrite outputs. */
    adc_empty = false;
    overflow = true;
    assert(read_window(&reading) != ESP_OK);
    overflow = false;
    corrupt = true;
    assert(read_window(&reading) == ESP_ERR_INVALID_RESPONSE);
    corrupt = false;
    current_counts = 100;
    assert(read_window(&reading) == ESP_OK); /* Driver recovers after errors. */

    reset_filters();
    current_counts = 0;
    run_task(2, 1);
    near(snapshots[0].corrente_rms, 0, 0);
    near(snapshots[0].potencia_w, 0, 0);
    near(snapshots[1].corrente_rms, 0, 0);
    near(snapshots[1].energia_kwh, 0, 0);

    reset_filters();
    current_counts = 100;
    run_task(10, 0);
    near(snapshots[9].corrente_pre_correcao_a, 100.0 * 3.3 / 4095 * 77.549, 0.00001);
    near(snapshots[0].energia_kwh, 0, 0);
    double expected_energy = 0;
    for (int k = 1; k < 10; ++k) {
        {
            expected_energy += 0.5 * ((double)snapshots[k-1].potencia_w + snapshots[k].potencia_w)
                             * (snapshot_times[k] - snapshot_times[k-1]) / 3.6e12;
        }
    }
    if (expected_energy < 0.0) { expected_energy = 0.0; }
    near(snapshots[9].energia_kwh, expected_energy, 1e-9);
    near(snapshots[9].corrente_rms, 6.132684708, 0.0002); /* No second baseline discount in task. */
    near(snapshots[9].potencia_w, 639.252136230, 0.001);
    near(snapshots[9].potencia_va, snapshots[9].tensao_rms * snapshots[9].corrente_rms, 0.0001);
    near(snapshots[9].fator_potencia, snapshots[9].potencia_w / snapshots[9].potencia_va, 0.00001);

    run_task(2, 3);
    near(snapshots[1].energia_kwh, 0.639252136230, 0.0000001); /* 639.252 W for one hour. */

    run_task(4, 2);
    assert(snapshots[1].sct_res == ESP_FAIL);
    near(snapshots[1].corrente_rms, snapshots[0].corrente_rms, 0); /* Error is not a zero measurement. */
    near(snapshots[2].energia_kwh, snapshots[0].energia_kwh, 0);
    assert(snapshots[3].energia_kwh > snapshots[2].energia_kwh);

    boot_energy = 12.510;
    run_task(4, 2);
    near(snapshots[0].energia_kwh, boot_energy, 0);
    near(snapshots[1].energia_kwh, boot_energy, 0); /* Failed ADC retains boot value. */
    near(snapshots[2].energia_kwh, boot_energy, 0); /* No integration across failure. */
    assert(snapshots[3].energia_kwh > boot_energy);
    boot_energy = 0;

    app_shared_data_t shared = {0};
    char *payload = telemetria_montar_json(&snapshots[3], &shared);
    assert(payload != NULL);
    cJSON *json = cJSON_Parse(payload);
    cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "dados");
    assert(cJSON_GetArraySize(json) == 5);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(json, "device")->valuestring, "esp32-energia") == 0);
    assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(json, "uptime_ms")));
    cJSON *status = cJSON_GetObjectItemCaseSensitive(json, "status");
    assert(cJSON_IsObject(status) && cJSON_GetArraySize(status) == 6);
    near(cJSON_GetObjectItemCaseSensitive(status, "wifi_rssi")->valuedouble, -57, 0);
    near(cJSON_GetObjectItemCaseSensitive(status, "heap_livre_bytes")->valuedouble, fake_heap_free_bytes, 0);
    near(cJSON_GetObjectItemCaseSensitive(status, "stack_min_bytes")->valuedouble, fake_stack_min_bytes, 0);
    assert(cJSON_IsObject(data) && cJSON_GetArraySize(data) == 10);
    const char *keys[] = {"corrente_rms_a", "tensao_rms_v", "potencia_ativa_w", "fator_potencia", "energia_kwh",
        "potencia_va", "potencia_reativa_var", "temperatura_c", "temperatura_ambiente_c", "umidade_pct"};
    for (unsigned k = 0; k < 10; ++k) { assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(data, keys[k]))); }
    near(cJSON_GetObjectItemCaseSensitive(data, "corrente_rms_a")->valuedouble, snapshots[3].corrente_rms, 0.00001);
    near(cJSON_GetObjectItemCaseSensitive(data, "tensao_rms_v")->valuedouble, snapshots[3].tensao_rms, 0.00001);
    near(cJSON_GetObjectItemCaseSensitive(data, "potencia_ativa_w")->valuedouble, snapshots[3].potencia_w, 0.00001);
    near(cJSON_GetObjectItemCaseSensitive(data, "fator_potencia")->valuedouble, snapshots[3].fator_potencia, 0.0000001);
    near(cJSON_GetObjectItemCaseSensitive(data, "energia_kwh")->valuedouble, snapshots[3].energia_kwh, 0.0000001);
    near(cJSON_GetObjectItemCaseSensitive(data, "potencia_va")->valuedouble, snapshots[3].potencia_va, 0.00001);
    near(cJSON_GetObjectItemCaseSensitive(data, "potencia_reativa_var")->valuedouble, snapshots[3].potencia_reativa_var, 0.00001);
    near(cJSON_GetObjectItemCaseSensitive(data, "temperatura_c")->valuedouble, snapshots[3].temperatura, 0.00001);
    near(cJSON_GetObjectItemCaseSensitive(data, "temperatura_ambiente_c")->valuedouble, snapshots[3].temperatura_ambiente, 0.00001);
    near(cJSON_GetObjectItemCaseSensitive(data, "umidade_pct")->valuedouble, snapshots[3].umidade, 0.00001);
    cJSON_Delete(json);
    payload = telemetria_montar_json(&snapshots[1], &shared);
    assert(payload == NULL); /* A preserved value from a failed ADC is not current. */
    /* Below-baseline noise clears loaded filters immediately. */
    current_counts = 19;
    half_count_noise = false;
    run_task(10, 0);
    for (int k = 0; k < 10; ++k) {
        near(snapshots[k].corrente_rms, 0, 0);
        near(snapshots[k].potencia_w, 0, 0);
        near(snapshots[k].potencia_va, 0, 0);
        near(snapshots[k].energia_kwh, 0, 0);
    }
    /* A coherent signal below the former 2.45 A raw gate must survive. */
    current_counts = 20;
    run_task(10, 0);
    near(snapshots[9].corrente_rms, 0.342609078, 0.0002);
    assert(snapshots[9].potencia_w > 0.0f);
    current_counts = 100;
    run_task(10, 0);
    run_task(2, 1); /* Load disappears: retain the final half-trapezoid. */
    near(snapshots[1].potencia_w, 0, 0);
    near(snapshots[1].energia_kwh,
         0.5 * snapshots[0].potencia_w * (snapshot_times[1] - snapshot_times[0]) / 3.6e12, 1e-12);
    current_counts = 100;
    phase_quarters = 2;
    run_task(10, 0);
    near(snapshots[9].potencia_w, 0, 0);
    near(snapshots[9].fator_potencia, 0, 0);
    near(snapshots[9].energia_kwh, 0, 0);
    phase_quarters = 0;
    current_counts = 100;
    assert(read_window(&reading) == ESP_OK);
    float clean_current = reading.corrente_rms;
    float clean_power = reading.potencia_w;
    rf_impulse = true;
    assert(read_window(&reading) == ESP_OK);
    near(reading.corrente_rms, clean_current, 0.001);
    near(reading.potencia_w, clean_power, 0.01);
    rf_impulse = false;
    rf_heavy = true;
    assert(read_window(&reading) == ESP_ERR_INVALID_RESPONSE);
    near(reading.potencia_w, clean_power, 0.01); /* A heavily corrupt window is not integrated. */
    rf_heavy = false;
    sine_wave = true;
    voltage_counts = 1000;
    current_counts = 500;
    rf_voltage_impulse = true;
    assert(read_window(&reading) == ESP_OK);
    /* Analytic fixture after excluding pair 51 from both 600-sample arrays.
     * Detects removing only V, using 600 as divisor, or shifting the I indices. */
    near(reading.tensao_rms, 361.966705322, 0.001);
    near(reading.corrente_rms, 22.054279327, 0.001);
    near(reading.potencia_w, 4001.93432617, 0.01);
    rf_voltage_impulse = false;
    sine_wave = false;

    memset(&test_runtime, 0, sizeof(test_runtime));
    test_runtime.mutex = (void *)1;
    ultima_medicao.t_leitura_us = now_us;
    ultima_medicao.resultado = ESP_OK;
    in_send = true;
    if (setjmp(task_exit) == 0) { task_envio_telemetria(&test_runtime); }
    in_send = false;
    near(send_delays[0], 5000, 0);
    near(send_delays[1], 10000, 0);
    near(send_delays[2], 500, 0);
    near(send_delays[3], 5000, 0);
    for (int k = 1; k < 4; ++k) {
        near(send_started_us[k] - send_finished_us[k-1], (double)send_delays[k-1] * 1000, 0);
    }
    assert(test_runtime.shared.total_envios_ok == 1 && test_runtime.shared.total_envios_erro == 3);
    test_json_allocation_failures();
    test_measurement_freshness();
    puts("PASS: DMA/DC, paired RF rejection, noise gate, phase/power/energy, mutex timeout, core pinning, JSON, HTTP 5s/10s backoff and recovery");
    return 0;
}
