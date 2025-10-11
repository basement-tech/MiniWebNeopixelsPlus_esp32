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

/*
 * see if the script needs to move to the next step
 */
int8_t script_update(bool new_data)  {
    char filename[MAX_FILENAME];  // the filename of the script file
    neo_script_cmd_t cmd_type = NEO_CMD_SCRIPT_UNDEFINED;

    switch(script_state)  {

        case NEO_SCRIPT_STOPPED:  // no current activity  ... waiting for signal to start
            if(new_data == true)  {
                /*
                 * copy the data so as not to hold the mutex too long
                 */
                if(xSemaphoreTake(xscriptMutex, 0) == pdTRUE)  {
                    cmd_type = script_mutex_data.cmd_type;
                    strncpy(filename, script_mutex_data.filename, sizeof(filename));
                    new_data = false;
                    xSemaphoreGive(xscriptMutex);
                }
                /*
                 *_START is the only valid command from _STOP
                 */
                if(cmd_type != NEO_CMD_SCRIPT_START)
                    ESP_LOGI(TAG, "invalid cmd_type received while stopped ... ignored");
                else  {
                    ESP_LOGI(TAG, "starting new script %s from STOPPED", filename);
                    script_state = NEO_SCRIPT_START;
                }
            }
            break;

        case NEO_SCRIPT_STOPPING:  // cleaning up and stopping
            script_state = NEO_SCRIPT_STOPPED;
            xSemaphoreGive(xscript_running_flag);  // last action
            break;

        case NEO_SCRIPT_START:  // initializing new script and sending first step

            if(xSemaphoreTake(xscript_running_flag, 0) != pdTRUE)
                ESP_LOGE(TAG, "Error taking xscript_running_flag at start of script");
            else  {

            }

            /*
             * start the new script (i.e. send the first sequence)
             */
            if(xSemaphoreTake(xneoMutex, 10/portTICK_PERIOD_MS) == pdFALSE)
                ESP_LOGE(TAG, "Failed to take mutex on initial sequence set ... no change");
            
            else  {
                strncpy(neo_mutex_data.sequence, "<newfile>", MAX_NEO_SEQUENCE);
                ESP_LOGI(TAG, "%s to be sent as initial sequence", neo_mutex_data.sequence);
                neo_mutex_data.file[0] = '\0';  // default sequence has to be a built-in
                neo_mutex_data.new_data = false;
                xSemaphoreGive(xneoMutex);
            }
            script_state = NEO_SCRIPT_STOPPING;
            break;
        
        case NEO_SCRIPT_WAIT:  // waiting for signal that step has completed, send next step
            script_state = NEO_SCRIPT_STOPPING;
            break;
        
        default:
            ESP_LOGD(TAG, "Invalid State");  //thread safety test
            break;
    }

    return (NEO_SUCCESS);
}
