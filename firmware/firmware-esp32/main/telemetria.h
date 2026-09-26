#pragma once

#include "shared_data.h"

/* Returns a pointer into a static buffer (not heap-allocated -- do not
 * free it), valid until the next call. Single caller (task_envio_telemetria),
 * called sequentially, so there is no concurrent-access hazard. */
char *telemetria_montar_json(const app_medicoes_t *medicoes, const app_shared_data_t *shared);
