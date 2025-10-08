/*
 * script engine
 */
#include "stdint.h"
#include "string.h"
#include "esp_log.h"

#include "neo_script.h"
#include "neo_data.h"

static const char *TAG = "neo_script";

static uint8_t script_state = NEO_SCRIPT_STOPPED;

/*
 * see if the script needs to move to the next step
 */
int8_t script_update(bool new_data)  {
    char filename[MAX_FILENAME];
    neo_script_cmd_t cmd_type = NEO_CMD_SCRIPT_UNDEFINED;

    switch(script_state)  {

        case NEO_SCRIPT_STOPPED:  // no current activity  ... waiting for signal to start
            if(new_data == true)  {
                if(xSemaphoreTake(xscriptMutex, 0) == pdTRUE)  {
                    cmd_type = script_mutex_data.cmd_type;
                    strncpy(filename, script_mutex_data.filename, sizeof(filename));
                    new_data = false;
                    xSemaphoreGive(xscriptMutex);
                }
                if(cmd_type != NEO_CMD_SCRIPT_START)
                    ESP_LOGI(TAG, "invalid cmd_type received while stopped");
                else  {
                    ESP_LOGI(TAG, "starting new script from STOPPED");
                    script_state = NEO_SCRIPT_START;
                }
            }
            break;

        case NEO_SCRIPT_STOPPING:  // cleaning up and stopping
        
            break;

        case NEO_SCRIPT_START:  // initializing new script and sending first step
        
            break;
        
        case NEO_SCRIPT_WAIT:  // waiting for signal that step has completed, send next step
        
            break;
        
        default:
            ESP_LOGD(TAG, "Invalid State");  //thread safety test
            break;
    }

    return (NEO_SUCCESS);
}
