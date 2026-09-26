#pragma once

#include "esp_err.h"

esp_err_t sensores_ambiente_inicializar(void);
esp_err_t sensores_ambiente_ler(float *umidade, float *temperatura);
esp_err_t sensores_ambiente_ler_temperatura_interna(float *temperatura);
