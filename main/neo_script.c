/*
 * script engine
 */
#include "stdint.h"
#include "esp_log.h"

#include "neo_script.h"
#include "neo_data.h"

static const char *TAG = "neo_script";

static uint8_t script_state = NEO_SCRIPT_STOPPED;

/*
 * see if the script needs to move to the next step
 */
int8_t script_update(void)  {

    switch(script_state)  {

        case NEO_SCRIPT_STOPPED:
        
            break;

        case NEO_SCRIPT_STOPPING:
        
            break;

        case NEO_SCRIPT_START:
        
            break;
        
        case NEO_SCRIPT_WAIT:
        
            break;
        
        case NEO_SCRIPT_WRITE:
        
            break;

        default:
            ESP_LOGD(TAG, "Invalid State");  //thread safety test
            break;
    }

    return (NEO_SUCCESS);
}
