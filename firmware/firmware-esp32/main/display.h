#pragma once

#include "esp_err.h"

#include "shared_data.h"

esp_err_t display_inicializar(app_handles_t *handles);
enum tela_atual {
    TELA_ELETRICA_BASICA = 0,
    TELA_POTENCIA_ENERGIA,
    TELA_REDE_DIAGNOSTICO,
};

void display_mostrar_debug(enum tela_atual *tela,
						   const app_shared_data_t *shared,
						   const app_medicoes_t *medicoes,
						   ssd1306_handle_t oled_handle);
