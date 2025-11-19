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

#include "lwip/ip_addr.h"

#define NEO_TASK_HANDLE_NAME "neo_process"
#define SERVO_TASK_HANDLE_NAME "servo_process"
#define SCRIPT_TASK_HANDLE_NAME "script_process"

/*
 * set task priorities here
 * note: task priorities range from 0 to (configMAX_PRIORITIES-1) (expands to 25)
 */
#define NEO_TASK_PRIORITY    10
#define SERVO_TASK_PRIORITY  10
#define SCRIPT_TASK_PRIORITY 10

# define __TB_WBANYWAY__

#endif