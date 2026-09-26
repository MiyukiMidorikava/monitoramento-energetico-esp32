#include <stdio.h>

#include "esp_netif.h"

#include "config_app.h"
#include "display.h"

/* A truncated reading must not look like a valid measurement on the OLED. */
#define DISPLAY_FORMAT(buffer, capacity, ...) do { \
    int written = snprintf((buffer), (capacity), __VA_ARGS__); \
    if (written < 0 || (size_t)written >= (capacity)) { \
        (buffer)[0] = '?'; \
        (buffer)[1] = '\0'; \
    } \
} while (0)

static void oled_mostrar_pagina(ssd1306_handle_t handle,
                                const char *l0,
                                const char *l1,
                                const char *l2,
                                const char *l3)
{
    ssd1306_clear_screen(handle, 0x00);
    const char *linhas[4] = {l0, l1, l2, l3};
    for (int i = 0; i < 4; i++) {
        if (linhas[i] != NULL && linhas[i][0] != '\0') {
            ssd1306_draw_string(handle,
                                0,
                                i * OLED_FONTE_PIXELS,
                                (const uint8_t *)linhas[i],
                                OLED_FONTE_PIXELS,
                                1);
        }
    }
    ssd1306_refresh_gram(handle);
}

esp_err_t display_inicializar(app_handles_t *handles)
{
    if (handles == NULL || handles->i2c_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    handles->oled_handle = ssd1306_create(handles->i2c_bus, SSD1306_I2C_ADDRESS);
#pragma GCC diagnostic pop

    if (handles->oled_handle == NULL) {
        return ESP_FAIL;
    }

    esp_err_t ret = ssd1306_init(handles->oled_handle);
    if (ret != ESP_OK) {
        ssd1306_delete(handles->oled_handle);
        handles->oled_handle = NULL;
        return ret;
    }

    ssd1306_clear_screen(handles->oled_handle, 0x00);
    ssd1306_refresh_gram(handles->oled_handle);
    return ESP_OK;
}

void display_mostrar_debug(enum tela_atual *tela,
                          const app_shared_data_t *shared,
                          const app_medicoes_t *medicoes,
                          ssd1306_handle_t oled_handle)
{
    if (tela == NULL || shared == NULL || medicoes == NULL || oled_handle == NULL) {
        return;
    }

    /* A fonte de 16 px tem caracteres de 8 px: 16 colunas no OLED de 128 px. */
    char l0[17] = {0};
    char l1[17] = {0};
    char l2[17] = {0};
    char l3[17] = {0};

    switch (*tela) {
        default:
            *tela = TELA_ELETRICA_BASICA;
            /* fall through */
        case TELA_ELETRICA_BASICA:
            DISPLAY_FORMAT(l0, sizeof(l0), "ELETRICA");
            if (medicoes->zmpt_res == ESP_OK && medicoes->sct_res == ESP_OK) {
                DISPLAY_FORMAT(l1, sizeof(l1), "V  %.1f V", medicoes->tensao_rms);
                DISPLAY_FORMAT(l3, sizeof(l3), "I  %.3f A", medicoes->corrente_rms);
            } else {
                DISPLAY_FORMAT(l1, sizeof(l1), "V  -- V");
                DISPLAY_FORMAT(l3, sizeof(l3), "I  -- A");
            }
            break;

        case TELA_POTENCIA_ENERGIA:
            DISPLAY_FORMAT(l0, sizeof(l0), "POTENCIA/ENERGIA");
            if (medicoes->zmpt_res == ESP_OK && medicoes->sct_res == ESP_OK) {
                DISPLAY_FORMAT(l1, sizeof(l1), "P  %.1f W", medicoes->potencia_w);
                DISPLAY_FORMAT(l2, sizeof(l2), "FP %.2f", medicoes->fator_potencia);
            } else {
                DISPLAY_FORMAT(l1, sizeof(l1), "P  -- W");
                DISPLAY_FORMAT(l2, sizeof(l2), "FP --");
            }
            DISPLAY_FORMAT(l3, sizeof(l3), "E %.3f kWh", medicoes->energia_kwh);
            break;

        case TELA_REDE_DIAGNOSTICO: {
            EventBits_t bits = shared->wifi_event_group != NULL
                ? xEventGroupGetBits(shared->wifi_event_group) : 0;
            bool conectado = (bits & WIFI_CONNECTED_BIT) != 0;
            esp_netif_ip_info_t ip_info = {0};
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            DISPLAY_FORMAT(l0, sizeof(l0), "Wi-Fi: %s", conectado ? "ON" : "OFF");
            DISPLAY_FORMAT(l2, sizeof(l2), "IP");
            if (conectado && netif != NULL &&
                esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
                DISPLAY_FORMAT(l3, sizeof(l3), IPSTR, IP2STR(&ip_info.ip));
            } else {
                DISPLAY_FORMAT(l3, sizeof(l3), "--");
            }
            break;
        }
    }

    /* Limpa apenas o buffer RAM e transmite uma unica vez, ja completo. */
    oled_mostrar_pagina(oled_handle, l0, l1, l2, l3);
    *tela = (enum tela_atual)((*tela + 1) % 3);
}
