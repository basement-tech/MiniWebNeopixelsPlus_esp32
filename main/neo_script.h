/*
 * script engine
 */
#ifndef __NEO_SCRIPT_H__
#define __NEO_SCRIPT_H__

#include "stdint.h"

#define NEO_SCRIPT_STOPPED  0
#define NEO_SCRIPT_STOPPING 1
#define NEO_SCRIPT_START    2
#define NEO_SCRIPT_WAIT     3
#define NEO_SCRIPT_WRITE    4

int8_t script_update(void);

#endif