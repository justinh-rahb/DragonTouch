#pragma once

#include <stdio.h>

#define ESP_LOGI(tag, format, ...) \
    fprintf(stderr, "I (%s): " format "\n", tag, ##__VA_ARGS__)
