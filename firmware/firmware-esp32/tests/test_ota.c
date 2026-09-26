#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_MONITOR_API_TOKEN "test-only-ota-token"
#include "../main/ota_app.c"

static uint8_t firmware[2048], written[4096];
static bool auth_header_set, fail_auth_header, fail_signature_header;
static size_t cursor, written_size;
static int status, aborted, selected, cleaned, begun;
static bool complete, same_image, no_partition, bad_end, bad_write, short_body;
static int64_t content_length;
/* A second esp_http_client "session" is opened for the detached .sig file;
 * these mocks are inherently global/sequential already (they ignore the
 * handle value), so route based on which URL was last opened instead of
 * threading a real session object through. */
static bool sessao_assinatura;
static uint8_t assinatura_bytes[128] = {1, 2, 3, 4};
static size_t assinatura_tamanho = 64, assinatura_cursor;
static int status_assinatura;
static int64_t content_length_assinatura;
static bool assinatura_valida, chave_publica_falha;
static const esp_partition_t passive = {.size = 4096, .label = "ota_1"};
static esp_app_desc_t current;
static esp_ota_img_states_t boot_state = ESP_OTA_IMG_VALID;
static int marks, rollbacks, restarts;
static bool mark_fails;
static int64_t now_us;
int64_t esp_timer_get_time(void) { return now_us; }
const esp_partition_t *esp_ota_get_running_partition(void) { return &passive; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *s)
{ (void)p; *s = boot_state; return ESP_OK; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{ ++marks; return mark_fails ? ESP_FAIL : ESP_OK; }
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void)
{ ++rollbacks; return ESP_FAIL; }

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p)
{ (void)p; return no_partition ? NULL : &passive; }
const esp_app_desc_t *esp_app_get_description(void)
{ current.app_elf_sha256[0] = same_image ? 1 : 0; return &current; }
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t size, esp_ota_handle_t *h)
{ assert(p == &passive && size == OTA_WITH_SEQUENTIAL_WRITES); *h = 1; ++begun; return ESP_OK; }
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *data, size_t n)
{ assert(h == 1 && written_size + n <= sizeof(written));
  if (bad_write) { return ESP_FAIL; }
  memcpy(written + written_size, data, n); written_size += n; return ESP_OK; }
esp_err_t esp_ota_end(esp_ota_handle_t h)
{ assert(h == 1); return bad_end ? ESP_FAIL : ESP_OK; }
esp_err_t esp_ota_abort(esp_ota_handle_t h)
{ assert(h == 1); ++aborted; return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{ assert(p == &passive && !bad_end && complete); ++selected; return ESP_OK; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c)
{
    assert(c->timeout_ms > 0);
    assert(c->disable_auto_redirect);
    auth_header_set = false;
    sessao_assinatura = strstr(c->url, ".sig") != NULL;
    if (sessao_assinatura) { assinatura_cursor = 0; }
    return (void *)1;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *name, const char *value)
{ (void)c; assert(strcmp(name, "X-API-Key") == 0); assert(strcmp(value, API_TOKEN) == 0);
  auth_header_set = !(fail_auth_header || (sessao_assinatura && fail_signature_header));
  return auth_header_set ? ESP_OK : ESP_FAIL; }
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int n)
{ (void)c; assert(n == 0); assert(auth_header_set); return ESP_OK; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{ (void)c; return sessao_assinatura ? content_length_assinatura : content_length; }
int esp_http_client_get_status_code(esp_http_client_handle_t c)
{ (void)c; return sessao_assinatura ? status_assinatura : status; }
int esp_http_client_read(esp_http_client_handle_t c, char *dest, int n)
{
    (void)c;
    if (sessao_assinatura) {
        size_t available = assinatura_tamanho - assinatura_cursor;
        if ((size_t)n > available) { n = (int)available; }
        memcpy(dest, assinatura_bytes + assinatura_cursor, (size_t)n);
        assinatura_cursor += (size_t)n;
        return n;
    }
    size_t available = (short_body ? 12 : sizeof(firmware)) - cursor;
    if ((size_t)n > available) { n = available; }
    if (n > 17) { n = 17; } /* Force fragmented headers and body. */
    memcpy(dest, firmware + cursor, n); cursor += n; return n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c)
{ (void)c; return complete; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{ (void)c; ++cleaned; return ESP_OK; }
void vTaskDelay(TickType_t t) { assert(t > 0); }
void esp_restart(void) { ++restarts; }
bool wifi_app_esta_conectado(const app_shared_data_t *s) { (void)s; return false; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "error"; }

/* mbedtls itself isn't ESP-IDF-specific, but pulling the real library into
 * this lightweight host harness is out of scope; these stubs only need to
 * exercise ota_app.c's control flow (download-then-verify-then-select),
 * not real cryptography -- that is validated on-device (see OTA.md). */
void mbedtls_sha256_init(mbedtls_sha256_context *ctx) { (void)ctx; }
int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224) { (void)ctx; (void)is224; return 0; }
int mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *input, size_t ilen)
{ (void)ctx; (void)input; (void)ilen; return 0; }
int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char output[32])
{ (void)ctx; memset(output, 0, 32); return 0; }
void mbedtls_sha256_free(mbedtls_sha256_context *ctx) { (void)ctx; }
void mbedtls_pk_init(mbedtls_pk_context *ctx) { (void)ctx; }
int mbedtls_pk_parse_public_key(mbedtls_pk_context *ctx, const unsigned char *key, size_t keylen)
{ (void)ctx; (void)key; (void)keylen; return chave_publica_falha ? -1 : 0; }
int mbedtls_pk_verify(mbedtls_pk_context *ctx, mbedtls_md_type_t md_alg,
                       const unsigned char *hash, size_t hash_len,
                       const unsigned char *sig, size_t sig_len)
{ (void)ctx; (void)md_alg; (void)hash; (void)hash_len; (void)sig; (void)sig_len;
  return assinatura_valida ? 0 : -1; }
void mbedtls_pk_free(mbedtls_pk_context *ctx) { (void)ctx; }

static void reset(void)
{
    cursor = written_size = 0;
    fail_auth_header = false;
    fail_signature_header = false;
    status = 200;
    aborted = selected = cleaned = begun = 0;
    complete = true;
    same_image = no_partition = bad_end = bad_write = short_body = false;
    content_length = sizeof(firmware);
    memset(firmware, 0, sizeof(firmware));
    firmware[0] = ESP_IMAGE_HEADER_MAGIC;
    esp_app_desc_t desc = {.magic_word = ESP_APP_DESC_MAGIC_WORD, .app_elf_sha256 = {1}};
    memcpy(firmware + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), &desc, sizeof(desc));

    assinatura_cursor = 0;
    assinatura_tamanho = 64;
    status_assinatura = 200;
    content_length_assinatura = (int64_t)assinatura_tamanho;
    assinatura_valida = true;
    chave_publica_falha = false;
}

int main(void)
{
    assert(ota_app_inicializar() == ESP_OK);
    ota_app_registrar_telemetria(ESP_OK, 200);
    ota_validar_boot();
    assert(marks == 0);
    boot_state = ESP_OTA_IMG_PENDING_VERIFY;
    assert(ota_app_inicializar() == ESP_OK);
    assert(ota_verificar_e_atualizar() == ESP_ERR_INVALID_STATE);
    for (int i = 0; i < 9; ++i) { ota_app_registrar_telemetria(ESP_OK, 200); }
    ota_validar_boot();
    assert(marks == 0);
    ota_app_registrar_telemetria(ESP_OK, 201);
    for (int i = 0; i < 9; ++i) { ota_app_registrar_telemetria(ESP_OK, 200); }
    ota_validar_boot();
    assert(marks == 0);
    ota_app_registrar_telemetria(ESP_FAIL, 200);
    for (int i = 0; i < 9; ++i) { ota_app_registrar_telemetria(ESP_OK, 200); }
    ota_validar_boot();
    assert(marks == 0);
    ota_app_registrar_telemetria(ESP_OK, 200);
    mark_fails = true;
    ota_app_registrar_telemetria(ESP_FAIL, -1); /* Ten successes already completed. */
    ota_validar_boot();
    assert(marks == 1 && atomic_load(&validacao_pendente));
    mark_fails = false;
    ota_validar_boot();
    assert(marks == 2 && !atomic_load(&validacao_pendente));
    ota_validar_boot();
    assert(marks == 2);
    assert(ota_app_inicializar() == ESP_OK);
    now_us = 119999999;
    ota_validar_boot();
    assert(rollbacks == 0);
    now_us = 120000000;
    ota_validar_boot();
    assert(rollbacks == 1 && restarts == 1);
    assert(ota_app_inicializar() == ESP_OK);
    now_us += 119999999;
    for (int i = 0; i < 10; ++i) { ota_app_registrar_telemetria(ESP_OK, 200); }
    ++now_us; /* Owner polls at deadline, after timely success. */
    ota_validar_boot();
    assert(marks == 3 && rollbacks == 1 && !atomic_load(&validacao_pendente));
    assert(ota_app_inicializar() == ESP_OK);
    now_us += 120000000;
    for (int i = 0; i < 10; ++i) { ota_app_registrar_telemetria(ESP_OK, 200); }
    ota_validar_boot();
    assert(marks == 3 && rollbacks == 2); /* Late success cannot accept image. */
    boot_state = ESP_OTA_IMG_VALID;
    assert(ota_app_inicializar() == ESP_OK);
    puts("PASS: OTA pending boot, 10 exact HTTP 200, broken streak, commit retry, timeout rollback");
    reset();
    assert(ota_verificar_e_atualizar() == ESP_OK);
    /* One esp_http_client session for the image, a second for the detached
     * .sig fetched right after esp_ota_end() succeeds. */
    assert(selected == 1 && cleaned == 2 && aborted == 0);
    assert(written_size == sizeof(firmware) && memcmp(written, firmware, sizeof(firmware)) == 0);
    reset(); content_length = 0; /* Chunked HTTP. */
    assert(ota_verificar_e_atualizar() == ESP_OK && selected == 1);
    reset(); same_image = true;
    assert(ota_verificar_e_atualizar() == ESP_ERR_NOT_FINISHED && begun == 0 && selected == 0);
    reset(); status = 404;
    assert(ota_verificar_e_atualizar() != ESP_OK && begun == 0 && cleaned == 1);
    reset(); fail_auth_header = true;
    assert(ota_verificar_e_atualizar() != ESP_OK && begun == 0 && cleaned == 1);
    reset(); fail_signature_header = true;
    assert(ota_verificar_e_atualizar() != ESP_OK && selected == 0 && cleaned == 2);
    reset(); complete = false;
    assert(ota_verificar_e_atualizar() != ESP_OK && aborted == 1 && selected == 0);
    reset(); ++content_length;
    assert(ota_verificar_e_atualizar() != ESP_OK && aborted == 1 && selected == 0);
    reset(); bad_end = true;
    assert(ota_verificar_e_atualizar() != ESP_OK && aborted == 0 && selected == 0);
    reset(); bad_write = true;
    assert(ota_verificar_e_atualizar() != ESP_OK && aborted == 1 && selected == 0);
    reset(); content_length = passive.size + 1;
    assert(ota_verificar_e_atualizar() != ESP_OK && begun == 0);
    reset(); short_body = true;
    assert(ota_verificar_e_atualizar() != ESP_OK && begun == 0);
    reset(); no_partition = true;
    assert(ota_verificar_e_atualizar() == ESP_ERR_NOT_FOUND && cleaned == 0);
    reset(); firmware[0] = 0;
    assert(ota_verificar_e_atualizar() != ESP_OK && begun == 0);
    reset(); atomic_flag_test_and_set(&atualizando);
    assert(ota_verificar_e_atualizar() == ESP_ERR_INVALID_STATE && begun == 0);
    atomic_flag_clear(&atualizando);
    assert(ota_verificar_e_atualizar() == ESP_OK);
    puts("PASS: OTA fragmented/chunked transfer, identical image, HTTP errors, truncation, size, write/validation failures, cleanup, concurrency");

    /* SHA256-diff alone only proves the image differs from what's running;
     * the signature gate must reject before ever selecting the new bank. */
    reset(); assinatura_valida = false;
    assert(ota_verificar_e_atualizar() != ESP_OK && begun == 1 && selected == 0);
    reset(); chave_publica_falha = true;
    assert(ota_verificar_e_atualizar() != ESP_OK && begun == 1 && selected == 0);
    reset(); status_assinatura = 404;
    assert(ota_verificar_e_atualizar() != ESP_OK && begun == 1 && selected == 0 && cleaned == 2);
    reset(); content_length_assinatura = 129; /* Exceeds the 128-byte cap. */
    assert(ota_verificar_e_atualizar() != ESP_OK && selected == 0);
    reset(); assinatura_tamanho = 20; content_length_assinatura = 20;
    assert(ota_verificar_e_atualizar() == ESP_OK && selected == 1); /* Any size under the cap is read in full. */
    puts("PASS: OTA signature gate accepts/rejects independently of the SHA256 diff check");
    return 0;
}
