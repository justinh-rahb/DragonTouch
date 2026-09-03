#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dt_portal_start(void);
esp_err_t dt_portal_factory_reset_from_ui(void);

#ifdef __cplusplus
}
#endif
