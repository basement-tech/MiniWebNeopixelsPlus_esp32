/*
 * script engine
 */
#include "stdint.h"
#include "string.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "neo_script.h"
#include "neo_data.h"

static const char *TAG = "neo_script";

static uint8_t script_state = NEO_SCRIPT_STOPPED;
static int16_t script_step = -1;  // current step in the script

/*
 * grab the mutex, set a command in the global/shared structure
 * and send the command/msg to the script engine
 * (e.g. neo process informing script process that a step has ended)
 */
BaseType_t send_script_msg(script_mutex_data_t msg)  {
    BaseType_t ret = pdFALSE;
    if((ret = xSemaphoreTake(xscriptMutex, 10)) == pdTRUE)  {
        memcpy(&script_mutex_data, &msg, sizeof(script_mutex_data));
        xSemaphoreGive(xscriptMutex);
    }
    return(ret);
}

/*
 * see if the script needs to move to the next step
 *
 * arguments:
 *  bool new_data  : did the calling program detect a new command
 *
 *  return: a NEO_* status to the calling program
 * 
 */

neo_script_step_t *script_steps;  // pointer to actual step data (for persistence)
uint8_t last_state = NEO_SCRIPT_UNDEFINED; // mostly for debugging
int8_t script_update()  {
    int8_t ret = NEO_SUCCESS;
    script_mutex_data_t script_cmd;

    script_cmd.new_data = false;
    script_cmd.cmd_type = NEO_CMD_SCRIPT_UNDEFINED;  // to detect if new command

    /*
     * poll the semaphore and copy the structure if we got a new command
     */
    if(xSemaphoreTake(xscriptMutex, 0) == pdTRUE)  {
        if(script_mutex_data.new_data == true)  {
            memcpy(&script_cmd, &script_mutex_data, sizeof(script_mutex_data_t));
            script_mutex_data.new_data = false; // in case it was set; so that cmd is processed once
        }
        xSemaphoreGive(xscriptMutex);
        if(script_cmd.new_data == true)
            ESP_LOGI(TAG, "new command %d received", script_cmd.cmd_type);
    }

    /*
     * display changes of state
     * (this function gets called in a fast loop)
     */
    if(script_state != last_state)  {
        ESP_LOGI(TAG, "script_state = %u", script_state);
        last_state = script_state;
    }

    /*
     * cycle the script engine state machine
     */
    switch(script_state)  {

        case NEO_SCRIPT_STOPPED:  // no current activity  ... waiting for signal to start

            /*
             *_START is the only valid command from _STOP
             */
            if (script_cmd.cmd_type == NEO_CMD_SCRIPT_START)  {
                ESP_LOGI(TAG, "starting new script from STOPPED");
                script_steps = script_cmd.steps;  // remember in case someone messes with it
                script_step = 0;  // start with the first step
                script_state = NEO_SCRIPT_START;
            }
            break;

        /*
         * ignore all new commands while stopping
         */
        case NEO_SCRIPT_STOPPING:  // cleaning up and moving to stopped
            if(script_steps != NULL)  {
                ESP_LOGI(TAG, "free()ing script step memory");
                free(script_steps);
                script_steps = NULL;
            }
            script_state = NEO_SCRIPT_STOPPED;
            xSemaphoreGive(xscript_running_flag);  // last action
            break;

        case NEO_SCRIPT_START:  // initializing new script and sending first step
            /*
             * start the new script (i.e. send the first sequence)
             */
            script_step = 0;
            if(xSemaphoreTake(xneoMutex, 10/portTICK_PERIOD_MS) == pdFALSE)
                ESP_LOGE(TAG, "Failed to take mutex on initial sequence set ... no change");
            
            else  {
                strncpy(neo_mutex_data.sequence, script_steps[script_step].label, MAX_NEO_SEQUENCE);
                strncpy(neo_mutex_data.file, script_steps[script_step].filename, MAX_FILENAME);
                neo_mutex_data.resp_reqd = false;  // this sequence not coming from web client
                neo_mutex_data.new_data = true;
                xSemaphoreGive(xneoMutex);
                ESP_LOGI(TAG, "sent step %d start label: %s, filename: %s to sequence engine",
                                script_step, neo_mutex_data.sequence, neo_mutex_data.file);
                script_state = NEO_SCRIPT_WAIT;
                xSemaphoreTake(xscript_running_flag, 10);  // script is running
            }
            break;
        
        case NEO_SCRIPT_WAIT:  // waiting for signal that step has completed, send next step
            if (script_cmd.cmd_type == NEO_CMD_SCRIPT_STEP_NEXT)  {
                script_step++;  // next step
                if(strcmp(script_steps[script_step].source, "end") == 0)  {  // last step?
                    script_state = NEO_SCRIPT_STOPPING;
                }
                else  {  // start the new step
                    strncpy(neo_mutex_data.sequence, script_steps[script_step].label, MAX_NEO_SEQUENCE);
                    strncpy(neo_mutex_data.file, script_steps[script_step].filename, MAX_FILENAME);
                    neo_mutex_data.resp_reqd = false;  // this sequence not coming from web client
                    neo_mutex_data.new_data = true;
                    xSemaphoreGive(xneoMutex);
                    ESP_LOGI(TAG, "sent next step %d start label: %s, filename: %s to sequence engine",
                                    script_step, neo_mutex_data.sequence, neo_mutex_data.file);
                    script_state = NEO_SCRIPT_WAIT;
                }
            }
            else if(script_cmd.cmd_type == NEO_CMD_SCRIPT_STOP_REQ)  {
                ESP_LOGI(TAG, "stop request received while script waiting");
                script_state = NEO_SCRIPT_STOPPING;
            }

            break;
        
        default:
            ESP_LOGD(TAG, "Invalid State");  //thread safety test
            break;
    }


    return (ret);
}
