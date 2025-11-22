#ifndef __MDI_PARSE_H__
#define __MDI_PARSE_H__

#include <stddef.h>
#include <stdint.h>

#define MDI_MAX_ARGS 4
#define MDI_MAX_ARG_SIZE 16
typedef struct {
    char *cmdstr;
    int8_t (*mdi_action)(void *arg);
} mdi_command_t;

int8_t mdi_master_action(char *cmd);

#endif