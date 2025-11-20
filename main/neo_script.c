/*
 * script engine
 */
#include "stdint.h"
#include "string.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rest_server.h"

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
BaseType_t neo_script_send_msg(script_mutex_data_t msg)  {
    BaseType_t ret = pdFALSE;
    if((ret = xSemaphoreTake(xscriptMutex, 10)) == pdTRUE)  {
        memcpy(&script_mutex_data, &msg, sizeof(script_mutex_data));
        xSemaphoreGive(xscriptMutex);
    }
    return(ret);
}

/*
 * return true if a script is running, otherwise false
 * arguments:
 *   blocktime : xBlockTime – The time in ticks to wait for the semaphore to become available.
 *                            The macro portTICK_PERIOD_MS can be used to convert this to a real time.
 *                            A block time of zero can be used to poll the semaphore.
 *                            A block time of portMAX_DELAY can be used to block indefinitely
 *                            (provided INCLUDE_vTaskSuspend is set to 1 in FreeRTOSConfig.h).
 */
bool neo_script_is_running(int blocktime)  {
    if(xSemaphoreTake(xscript_running_flag, blocktime) == pdTRUE)  {
        xSemaphoreGive(xscript_running_flag);
        return(false);
    }
    else
        return(true);
}

/*
 * send an update command to the script engine
 * adjust behavior based on whether a script is running
 */
BaseType_t neo_script_progress_msg(neo_script_cmd_t cmd)  {

  BaseType_t ret = pdFALSE;

  script_mutex_data_t script_cmd;
  script_cmd.cmd_type = cmd;
  script_cmd.new_data = true;
  script_cmd.steps = NULL;

  if(neo_script_is_running(0) == true)  {  // script is running: held by script engine
      if((ret = neo_script_send_msg(script_cmd)) == pdTRUE)
        ESP_LOGI(TAG, "script command (%d) sent successfully", script_cmd.cmd_type);
      else
        ESP_LOGE(TAG, "error sending script command (%d)", script_cmd.cmd_type);
  }

  return(ret);
}

/*
 * wait for the script engine to stop the script
 * note: this is, to some degree, blocking.
 */
BaseType_t neo_script_verify_stop(void)  {
  bool ret = true;
  int8_t timeouts = SCRIPT_STOP_INTERVALS;

  while((ret == true) && (timeouts > 0))  {
    ret = neo_script_is_running(SCRIPT_STOP_PER_INTERVAL);
    //ret = xSemaphoreTake(xscript_running_flag, portMAX_DELAY);
    //ESP_LOGI(TAG, "xSemaphoreTake() returned %s on iteration %d", ((ret == pdFALSE) ? "pdFALSE" : "pdTRUE"), timeouts);
    timeouts--;
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
int8_t neo_script_update()  {
    int8_t ret = NEO_SUCCESS;
    script_mutex_data_t script_cmd;
    char status_msg[128];  // updating the UI status line

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
            send_status_update("Status: Script Stopped");
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
            send_status_update("Status: Script Started");
            break;
        
        case NEO_SCRIPT_WAIT:  // waiting for signal that step has completed, send next step
            if (script_cmd.cmd_type == NEO_CMD_SCRIPT_STEP_NEXT)  {
                script_step++;  // next step
                if(strcmp(script_steps[script_step].source, "end") == 0)  {  // last step?
                    script_state = NEO_SCRIPT_STOPPING;
                }
                else  {  // start the new step
                    neo_request_sequence(script_steps[script_step].label, script_steps[script_step].filename);
                    ESP_LOGI(TAG, "sent NEXT step %d start label: %s, filename: %s to sequence engine",
                                    script_step, script_steps[script_step].label, script_steps[script_step].filename);
                    snprintf(status_msg, sizeof(status_msg), "Status: Moved to NEXT step (%d): %s",
                                                                script_step, script_steps[script_step].label);
                    send_status_update(status_msg);
                    script_state = NEO_SCRIPT_WAIT;
                }
            }
            else if(script_cmd.cmd_type == NEO_CMD_SCRIPT_STEP_PREV)  {
                script_step--; // previous step
                if(script_step < 0)
                    script_step = 0;
                neo_request_sequence(script_steps[script_step].label, script_steps[script_step].filename);
                ESP_LOGI(TAG, "sent PREV step %d start label: %s, filename: %s to sequence engine",
                                script_step, script_steps[script_step].label, script_steps[script_step].filename);
                snprintf(status_msg, sizeof(status_msg), "Status: Moved to PREVIOUS step (%d): %s",
                                                                script_step, script_steps[script_step].label);
                send_status_update(status_msg);
                script_state = NEO_SCRIPT_WAIT;
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
