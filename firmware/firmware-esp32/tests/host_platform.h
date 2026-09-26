#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NOT_FINISHED 0x10c
#define ESP_ERR_HTTP_CONNECT 0x7002
const char *esp_err_to_name(esp_err_t err);
int64_t esp_timer_get_time(void);
typedef struct { int8_t rssi; } wifi_ap_record_t;
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record);
static inline void host_log(const char *tag, const char *format, ...)
{ (void)tag; (void)format; }
#define ESP_LOGI(tag, ...) host_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_log(tag, __VA_ARGS__)

#define ADC_UNIT_1 0
#define ADC_CHANNEL_4 4
#define ADC_CHANNEL_5 5
#define ADC_ATTEN_DB_12 3
#define ADC_BITWIDTH_12 12
#define ADC_CONV_SINGLE_UNIT_1 1
#define ADC_DIGI_OUTPUT_FORMAT_TYPE2 2
#define SOC_ADC_DIGI_RESULT_BYTES 4
typedef void *adc_continuous_handle_t;
typedef struct { uint8_t atten, channel, unit, bit_width; } adc_digi_pattern_config_t;
typedef union {
    /* ESP32-S3 TYPE2: channel starts at bit 13, unit at bit 17. */
    struct { uint32_t data:12, reserved12:1, channel:4, unit:1, reserved:14; } type2;
    uint32_t val;
} adc_digi_output_data_t;
typedef struct {
    uint32_t max_store_buf_size, conv_frame_size;
    struct { uint32_t flush_pool:1; } flags;
} adc_continuous_handle_cfg_t;
typedef struct {
    uint32_t pattern_num;
    adc_digi_pattern_config_t *adc_pattern;
    uint32_t sample_freq_hz;
    int conv_mode, format;
} adc_continuous_config_t;
typedef struct { uint8_t *conv_frame_buffer; uint32_t size; } adc_continuous_evt_data_t;
typedef bool (*adc_continuous_callback_t)(adc_continuous_handle_t, const adc_continuous_evt_data_t *, void *);
typedef struct { adc_continuous_callback_t on_conv_done, on_pool_ovf; } adc_continuous_evt_cbs_t;
esp_err_t adc_continuous_new_handle(const adc_continuous_handle_cfg_t *, adc_continuous_handle_t *);
esp_err_t adc_continuous_config(adc_continuous_handle_t, const adc_continuous_config_t *);
esp_err_t adc_continuous_register_event_callbacks(adc_continuous_handle_t, const adc_continuous_evt_cbs_t *, void *);
esp_err_t adc_continuous_start(adc_continuous_handle_t);
esp_err_t adc_continuous_read(adc_continuous_handle_t, uint8_t *, uint32_t, uint32_t *, uint32_t);
esp_err_t adc_continuous_stop(adc_continuous_handle_t);
esp_err_t adc_continuous_flush_pool(adc_continuous_handle_t);
esp_err_t adc_continuous_deinit(adc_continuous_handle_t);

typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef void *ssd1306_handle_t;
typedef void *EventGroupHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
uint32_t esp_get_free_heap_size(void);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define portNUM_PROCESSORS 2
TickType_t xTaskGetTickCount(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
void vTaskDelay(TickType_t);
void vTaskDelayUntil(TickType_t *, TickType_t);
BaseType_t xTaskCreate(void (*)(void *), const char *, uint32_t, void *, unsigned, void *);
BaseType_t xTaskCreatePinnedToCore(void (*)(void *), const char *, uint32_t, void *, unsigned, void *, BaseType_t);
QueueHandle_t xQueueCreate(unsigned, unsigned);
BaseType_t xQueueOverwrite(QueueHandle_t, const void *);
BaseType_t xQueuePeek(QueueHandle_t, void *, TickType_t);
void vQueueDelete(QueueHandle_t);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskNotifyGiveFromISR(TaskHandle_t, BaseType_t *);
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t);
esp_err_t esp_task_wdt_add(TaskHandle_t);
esp_err_t esp_task_wdt_delete(TaskHandle_t);
esp_err_t esp_task_wdt_reset(void);

/* Real signatures/mbedtls types; only test_ota.c's stubs give these bodies. */
typedef struct { int dummy; } mbedtls_sha256_context;
typedef struct { int dummy; } mbedtls_pk_context;
typedef int mbedtls_md_type_t;
#define MBEDTLS_MD_SHA256 0
void mbedtls_sha256_init(mbedtls_sha256_context *);
int mbedtls_sha256_starts(mbedtls_sha256_context *, int);
int mbedtls_sha256_update(mbedtls_sha256_context *, const unsigned char *, size_t);
int mbedtls_sha256_finish(mbedtls_sha256_context *, unsigned char[32]);
void mbedtls_sha256_free(mbedtls_sha256_context *);
void mbedtls_pk_init(mbedtls_pk_context *);
int mbedtls_pk_parse_public_key(mbedtls_pk_context *, const unsigned char *, size_t);
int mbedtls_pk_verify(mbedtls_pk_context *, mbedtls_md_type_t, const unsigned char *, size_t, const unsigned char *, size_t);
void mbedtls_pk_free(mbedtls_pk_context *);

/* Host-only OTA transport/flash boundary used by test_ota.c. */
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_SIZE 0x104
#define CONFIG_MONITOR_OTA_URL "http://test/firmware.bin"
#define CONFIG_MONITOR_OTA_INTERVAL_SECONDS 3600
#define ESP_IMAGE_HEADER_MAGIC 0xe9
#define ESP_APP_DESC_MAGIC_WORD 0xabcd5432
#define OTA_WITH_SEQUENTIAL_WRITES 0xfffffffe
#define ESP_LOGE(tag, ...) host_log(tag, __VA_ARGS__)
typedef struct { uint8_t magic; uint8_t padding[23]; } esp_image_header_t;
typedef struct { uint32_t address, size; } esp_image_segment_header_t;
typedef struct { uint32_t magic_word; uint8_t app_elf_sha256[32]; } esp_app_desc_t;
typedef struct { uint32_t size; const char *label; } esp_partition_t;
typedef unsigned esp_ota_handle_t;
typedef void *esp_http_client_handle_t;
typedef struct { int event_id; const void *data; int data_len; } esp_http_client_event_t;
#define HTTP_EVENT_ON_CONNECTED 1
#define HTTP_EVENT_ON_DATA 2
#define HTTP_METHOD_POST 1
static inline esp_err_t esp_crt_bundle_attach(void *config) { (void)config; return ESP_OK; }
typedef struct { const char *url; int timeout_ms; bool keep_alive_enable;
    bool disable_auto_redirect;
    esp_err_t (*crt_bundle_attach)(void *); int method;
    esp_err_t (*event_handler)(esp_http_client_event_t *); } esp_http_client_config_t;
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t, const char *, int);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *);
const esp_app_desc_t *esp_app_get_description(void);
esp_err_t esp_ota_begin(const esp_partition_t *, size_t, esp_ota_handle_t *);
esp_err_t esp_ota_write(esp_ota_handle_t, const void *, size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_open(esp_http_client_handle_t, int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t, char *, int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
void esp_restart(void);
typedef enum { ESP_RST_UNKNOWN = 0, ESP_RST_TASK_WDT = 6 } esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason(void);

typedef unsigned nvs_handle_t;
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 0x1102
esp_err_t nvs_flash_init(void);
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
typedef enum { ESP_OTA_IMG_VALID, ESP_OTA_IMG_PENDING_VERIFY } esp_ota_img_states_t;
const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void);
#define IRAM_ATTR
#define ESP_ERROR_CHECK(expr) assert((expr) == ESP_OK)
