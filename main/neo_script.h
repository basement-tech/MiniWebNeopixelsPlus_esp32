/*
 * script engine
 */
#ifndef __NEO_SCRIPT_H__
#define __NEO_SCRIPT_H__

#include "stdint.h"
#include "neo_data.h"

#define NEO_SCRIPT_STOPPED  0
#define NEO_SCRIPT_STOPPING 1
#define NEO_SCRIPT_START    2
#define NEO_SCRIPT_WAIT     3
#define NEO_SCRIPT_WRITE    4



/*
 * script process startup and IPC communication constructs
 */
typedef struct  {
    char filename[MAX_FILENAME];
    bool new_data;   // true if new sequence request has been made
} script_mutex_data_t;

extern SemaphoreHandle_t xscriptMutex;  // used to protect communication to script engine
extern script_mutex_data_t script_mutex_data;  // data to be sent to script engine from neo_play

/*
 * public functions
 */
int8_t script_update(bool new_data);

#endif