#ifndef __NEO_SYSTEM_H__
#define __NEO_SYSTEM_H__

#include "esp_log.h"

#define NEO_DEBUG_LEVEL ESP_LOG_INFO
//#define NEO_DEBUG_LEVEL ESP_LOG_DEBUG
static inline void cli_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
}

#define CLI_PRINTF(...) cli_printf(__VA_ARGS__)

#define LITTLE_FS_MOUNT_POINT     "/littlefs"
#define LITTLE_FS_PARTITION_LABEL "files"

#endif