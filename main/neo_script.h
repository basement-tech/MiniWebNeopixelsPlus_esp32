/*
 * script engine
 */
#ifndef __NEO_SCRIPT_H__
#define __NEO_SCRIPT_H__

#include "stdint.h"
#include "neo_data.h"

#define NEO_SCRIPT_STOPPED   0
#define NEO_SCRIPT_STOPPING  1
#define NEO_SCRIPT_START     2
#define NEO_SCRIPT_WAIT      3
#define NEO_SCRIPT_WRITE     4
#define NEO_SCRIPT_UNDEFINED 5

typedef enum  {
    NEO_CMD_SCRIPT_START,      // start a new script please
    NEO_CMD_SCRIPT_STEP_NEXT,  // next step (e.g. seq ended or other)
    NEO_CMD_SCRIPT_STEP_PREV,  // previous step
    NEO_CMD_SCRIPT_STOP_REQ,   // stop the current script please
    NEO_CMD_SCRIPT_UNDEFINED   // for init and error checking
} neo_script_cmd_t;

/*
 * used for filetype SCRIPT parsing/storage
 */
#define SCRIPT_MAX_SOURCE_SIZE   8
#define SCRIPT_MAX_NAME_SIZE     32
#define SCRIPT_MAX_LABEL         16
#define SCRIPT_MAX_STEPS         64

/*
 * script engine timing parameters
 *
 * note: the update interval for the script engine in general, 
 * SCRIPT_UPDATE_INTERVAL is the fastest that a running script
 * will be shut down.  (SCRIPT_STOP_PER_INTERVAL * SCRIPT_STOP_INTERVALS)
 * is the total time that the pixel process will wait after asking it to 
 * shut down the script.  the wait-for-stop time should be adjusted with the
 * script engine interval in mind.
 */
#define SCRIPT_UPDATE_INTERVAL   (5/portTICK_PERIOD_MS)  // state machine update interval in mS
#define SCRIPT_STOP_PER_INTERVAL (2/portTICK_PERIOD_MS)  // mS to wait for stop confirm
#define SCRIPT_STOP_INTERVALS    10  // number of times to wait SCRIPT_STOP_PER_INTERVAL
typedef struct  {
  char source[SCRIPT_MAX_SOURCE_SIZE];  // "builtin", "file", "end"
  char label[SCRIPT_MAX_LABEL];         // label in sequence data array
  char filename[SCRIPT_MAX_NAME_SIZE];  // name of the file or builin
  int  repeat; // number of times to repeat the step (doesn't need to be int, but that's what json parser wants)
} neo_script_step_t;

/*
 * script process startup and IPC communication constructs
 */
typedef struct  {
    neo_script_cmd_t cmd_type;  // what is the neo process trying to say
    bool new_data;   // true if new sequence request has been made
    neo_script_step_t *steps;  // actual step data
} script_mutex_data_t;

extern SemaphoreHandle_t xscriptMutex;  // used to protect communication to script engine
extern script_mutex_data_t script_mutex_data;  // data to be sent to script engine from neo_play

extern SemaphoreHandle_t xscript_running_flag;  // true if a script is running (e.g. used to sync stop of script and start of seq)

/*
 * public functions
 */
int8_t     neo_script_update(void);                       // cycle the script engine state machine
BaseType_t neo_script_send_msg(script_mutex_data_t msg);  // send message to script engine
bool       neo_script_is_running(int blocktime);          // returns true if a script is running, false if not
BaseType_t neo_script_progress_msg(neo_script_cmd_t cmd); // send command-only (no data pointer) to script engine
BaseType_t neo_script_verify_stop(void);                  // wait for script running mutex to be available

#endif