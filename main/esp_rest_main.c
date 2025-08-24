/****
 * miniwebneopixelsplus
 * 
 * *** NOTE: although white("w")  appears in many data structure, it is not faithfully
 * implemented throughout.  As a matter of fact, as of this writing, it is not implemented
 * at all and will not function. ***
 * 
 * 
 * -> web client based control of neopixels and servos
 *    using an embedded web server and littlsfs
 * 
 * webserver functionality based on esp-idf HTTP Restful API Server Example
 *
 * This code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 * 
 * 
 * Web Server <-> neopixel engine data flow
 * ----------------------------------------
 * 
 *    Client               Webserver                Data Structure                   NeoPixel Process
 *    ------               ---------                ---------------                  ----------------
 * <Seq Button>    ->   button_handler() -> neo_mutex_data_t neo_mutex_data     ->   neo_new_sequence()
 *                                           SemaphoreHandle_t xneoMutex               (neo_play.c)
 *                                                  (neo_data.c/h)
 *                         (blocks)
 *                                                     (data)
 *                                       <- rest_resp_queue_t rest_resp_pending 
 *                                            SemaphoreHandle_t xrespMutex
 *                                                  (rest_server.c)
 * 201 or 405          button_handler()                                         <-  rest_response_setGo()
 *                                                    (signal)                        (rest_server.c)
 *                                       <- SemaphoreHandle_t xrespSemaphore
 *                                                 (rest_server.c)
 * Notes:
 * - this is a bit tighter coupling than I would have liked
 * - i implemented a mutex to insure that the button_handler() could
 *   only be executed one button at a time; it was never triggered,
 *   so I deleted it.
 * - I couldn't find a way to save the button push webserver context
 *   so that I could later associate a response with a button fetch request,
 *   but there must be a way.  Perhaps something in the header.
 * - based on above, it seems that the webserver queues button presses
 *   from the client, but eventually sends them all
 * - the client does seem to require a response to each fetch, otherwise
 *   it stays in "pending" state forever
 * - in testing, blocking the button_handler() did not trigger any
 *   starvation panics or other misbehavior
 *
 * 
 * Web Server
 * ----------
 * 
 * 
 * Filesystem
 * ----------
 * All development and testing done with littleFS.
 * I left in the other options for reference, but have not done any testing with them.
 * 
 * Configuration
 * -------------
 * Trying to preserve the use of esp-idf's Menuconfig functionality
 * for compiled in options and defaults.
 * i.e all default configuration is done through the interface in esp-idf.
 * command line eeprom parameters are set for user settings like number of neopixels.
 * 
 * Components/Libraries
 * --------------------
 * cJSON - used to serialize strings to json
 * espressif_json_parser - deserialize/parse json input
 * espressif_jsmn - espressif_json_parser is built on this
 * joltwallet_littlefs - littleFS embedded filesystem
 * espressif_mdns - mdns for hostname access via .local
 * From the build process:
 * NOTICE: Processing 6 dependencies:
 * NOTICE: [1/6] espressif/jsmn (1.1.0)
 * NOTICE: [2/6] espressif/json_parser (1.0.3)
 * NOTICE: [3/6] espressif/mdns (1.8.2)
 * NOTICE: [4/6] joltwallet/littlefs (1.19.1)
 * NOTICE: [5/6] protocol_examples_common (*) (C:\Users\djzma\esp\v5.4.1\esp-idf\examples\common_components\protocol_examples_common)
 * NOTICE: [6/6] idf (5.4.1)
 * 
 * Important esp-idf configuration items to set
 * --------------------------------------------
 * wifi ssid
 * wifi passwd
 * FREERTOS frequency to 1000 (from 100)
 * filesystem type to LittleFS
 * FreeRTOS item to expose process information
 * 
 * Timers
 * ------
 * https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/gptimer.html
 * https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/esp_timer.html
 * 
 * Todo:
 * o Implement multifile upload
 * o handle multiple sequence responses in a queue ... is there an index in the fetch header?
 * o rainbow sequence: finish port from adafruit, including 32bit stacked colors
 * o check all esp-idf ESP_ERROR_CHECK() fatal errors for sanity, possible change
 * o gamma32 : decide whether to implement or not, do it
 * o tie in all eeprom parameters
 * o softAP for configuration on hardware button
 * o decide whether to eliminate other fs options
 * o OTA : decide whether to implement, do it
 * 
 * 
 *
 * (c)djz 2025
 * 
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_vfs_semihost.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "sdmmc_cmd.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mdns.h"
#include "lwip/apps/netbiosns.h"
#include "station_example.h"

#include "neo_system.h"
#include "bt_eepromlib.h"
#include "rest_server.h"
#include "neo_ll_api.h"
#include "neo_data.h"

#include "driver/i2c_types.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "servo_defs.h"


#if CONFIG_EXAMPLE_WEB_DEPLOY_SD
#include "driver/sdmmc_host.h"
#endif

#if CONFIG_EXAMPLE_WEB_DEPLOY_LITTLE_FS
#include "esp_littlefs.h"
#endif

#define MDNS_INSTANCE "esp home web server"

static const char *TAG = "esp_rest_main";

net_config_t *pmon_config;
 

esp_err_t start_rest_server(const char *base_path);

static void initialise_mdns(void)
{
    mdns_init();
    mdns_hostname_set(CONFIG_EXAMPLE_MDNS_HOST_NAME);
    mdns_instance_name_set(MDNS_INSTANCE);

    mdns_txt_item_t serviceTxtData[] = {
        {"board", "esp32"},
        {"path", "/"}
    };

    ESP_ERROR_CHECK(mdns_service_add("ESP32-WebServer", "_http", "_tcp", 80, serviceTxtData,
                                     sizeof(serviceTxtData) / sizeof(serviceTxtData[0])));
}

#if CONFIG_EXAMPLE_WEB_DEPLOY_SEMIHOST
esp_err_t init_fs(void)
{
    esp_err_t ret = esp_vfs_semihost_register(CONFIG_EXAMPLE_WEB_MOUNT_POINT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register semihost driver (%s)!", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    return ESP_OK;
}
#endif

#if CONFIG_EXAMPLE_WEB_DEPLOY_SD
esp_err_t init_fs(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    gpio_set_pull_mode(15, GPIO_PULLUP_ONLY); // CMD
    gpio_set_pull_mode(2, GPIO_PULLUP_ONLY);  // D0
    gpio_set_pull_mode(4, GPIO_PULLUP_ONLY);  // D1
    gpio_set_pull_mode(12, GPIO_PULLUP_ONLY); // D2
    gpio_set_pull_mode(13, GPIO_PULLUP_ONLY); // D3

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(CONFIG_EXAMPLE_WEB_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }
    /* print card info if mount successfully */
    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}
#endif

#if CONFIG_EXAMPLE_WEB_DEPLOY_SF
esp_err_t init_fs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_EXAMPLE_WEB_MOUNT_POINT,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = false
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
    return ESP_OK;
}
#endif

#if CONFIG_EXAMPLE_WEB_DEPLOY_LITTLE_FS
esp_err_t init_fs(void)  {
    esp_vfs_littlefs_conf_t conf = {
        .base_path = LITTLE_FS_MOUNT_POINT,
        .partition_label = LITTLE_FS_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    // Use settings defined above to initialize and mount LittleFS filesystem.
    // Note: esp_vfs_littlefs_register is an all-in-one convenience function.
    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
        ESP_LOGI(TAG, "Attempting to format partition");
        esp_littlefs_format(conf.partition_label);
    } else {
        ESP_LOGI(TAG, "Filesystem Partition size: total: %d, used: %d", total, used);
    }

    return(ret);
}
#endif

/*
 * some stuff to study timing
 */
#define GPIO_OUTPUT_IO_0    12
#define GPIO_OUTPUT_IO_1    13
#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<GPIO_OUTPUT_IO_0) | (1ULL<<GPIO_OUTPUT_IO_1))

/*
 * ^^^^^^^^^^^^^^^^^^^^^^^^^^^
 * Let's say, GPIO_OUTPUT_IO_0=18, GPIO_OUTPUT_IO_1=19
 * In binary representation,
 * 1ULL<<GPIO_OUTPUT_IO_0 is equal to 0000000000000000000001000000000000000000 and
 * 1ULL<<GPIO_OUTPUT_IO_1 is equal to 0000000000000000000010000000000000000000
 * GPIO_OUTPUT_PIN_SEL                0000000000000000000011000000000000000000
 * */
static void gpio_init(void)  {
    //zero-initialize the config structure.
    gpio_config_t io_conf = {};
    //disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 1;  // enabled pull up  - DJZ
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    gpio_set_level(GPIO_OUTPUT_IO_1, 0);
}

/*
 * neopixel process
 */
SemaphoreHandle_t xneoMutex;  // used to protect communication to neo_play
neo_mutex_data_t neo_mutex_data;  // data to be sent to neo_play process from webserver

SemaphoreHandle_t xneo_cycle_next_flag;  // neo state machine cycle timer

#define NEO_TAG "neopixel_process"

static void neopixel_process(void *pvParameters)  {
    uint16_t count = atoi(pmon_config->neocount);
    int8_t err = NEO_SUCCESS;

    gpio_init();  // for debugging

    /*
     * obtain and display the task info for debugging
     *
     * FReeRTOS Note:
     * configUSE_TRACE_FACILITY must be defined as 1 for vTaskGetInfo() function to be available. 
     * See the configuration section (menuConfig in esp-idf) for more information.
     */
    TaskHandle_t xNeoTaskHandle;
    TaskStatus_t xTaskDetails;
    xNeoTaskHandle = xTaskGetCurrentTaskHandle();
    if(xNeoTaskHandle != NULL)  {
        vTaskGetInfo( xNeoTaskHandle,
                    &xTaskDetails,
                    pdTRUE, // Include the high water mark in xTaskDetails.
                    eInvalid ); // Include the task state in xTaskDetails.

        ESP_LOGI(TAG, "neopixel process started as \"%s (%d)\":", xTaskDetails.pcTaskName, xTaskDetails.xTaskNumber);
    }

    /*
     * create the binary semaphore that will
     * signal (ultimately) the state machine to cycle
     * (usually to move to the next pixel color in the sequence)
     * it is "given" by a timer that runs in neo_play.c.
     */
    if((xneo_cycle_next_flag = xSemaphoreCreateBinary()) == NULL)
        ESP_LOGE(NEO_TAG, "Error creating xneo_cycle_next_flag semaphore");
    else  {
        ESP_LOGI(NEO_TAG, "xneo_cycle_next_flag semaphore created successfully");
        xSemaphoreGive(xneo_cycle_next_flag);  // make it available
    }

    /*
     * create the binary mutex that will protect
     * the new sequence request data structure
     */
    xneoMutex = xSemaphoreCreateMutex();

    if(xneoMutex == NULL)  {
        ESP_LOGE(NEO_TAG, "Error creating neoMutex ... default sequence only");
    }
    else{
        ESP_LOGI(NEO_TAG, "neoMutex created successfully");
    }

    if(xSemaphoreTake(xneoMutex, 10/portTICK_PERIOD_MS) == pdFALSE)
        ESP_LOGE(NEO_TAG, "Failed to take mutex on initial sequence set ... no change");
    else  {
        strncpy(neo_mutex_data.sequence, pmon_config->neodefault, MAX_NEO_SEQUENCE);
        ESP_LOGI(NEO_TAG, "%s to be sent as initial sequence", neo_mutex_data.sequence);
        neo_mutex_data.file[0] = '\0';  // default sequence has to be a built-in
        neo_mutex_data.new_data = false;
        xSemaphoreGive(xneoMutex);
    }
    
    pixels_init();  // set up the low level neo_pixel RMT engine
    pixels_setcount(count);  // set the number of pixels in the strand
    ESP_LOGI(NEO_TAG, "Allocating array for %d pixels", count);
    pixels_alloc();  // allocate neo_pixel color array based on number of pixels
    neo_init();  // initialize strand parameters and ready the sequence state machine

    /*
     * kick-off the first/default sequence (based on eeprom parameter)
     */
    if(xSemaphoreTake(xneoMutex, 10/portTICK_PERIOD_MS) == pdFALSE)
        ESP_LOGE(NEO_TAG, "Failed to take mutex on initial sequence set ... no change");
    else  {
        neo_mutex_data.new_data = true;
        neo_mutex_data.resp_reqd = false;  // since not called from webserver
        xSemaphoreGive(xneoMutex);
    }

    while(1)  {
        /*
         * wait at most e.g. 200 mS for the cycle next flag.  After the timeout,
         * check to see if a new sequence was requested.  If a sequence is 
         * running, this might be very often.  If not, it will be at the timeout
         * interval and prevent the rtos from starving and panicing.
         */
        xSemaphoreTake(xneo_cycle_next_flag, NEO_CHK_NEWS_INTERVAL);  // wait for the signal from timer
        gpio_set_level(GPIO_OUTPUT_IO_1, 1);
        neo_cycle_next();  // cycle the neopixel state machine, noop if not running
        gpio_set_level(GPIO_OUTPUT_IO_1, 0);

        /*
         * check to see of a new sequence was requested
         * NOTE: a sequence triggered from a c function
         * (e.g. setting the initial sequence at startup)
         * will return NEO_NOR_SUCCESS and not require/trigger
         * a response to the web client.  NEO_SUCCESS indicates that
         * a button was not pressed (just polling).
         */
        err = neo_new_sequence();
        if(err == NEO_OLD_SUCCESS)
            rest_response_setGo(ESP_OK, "ignored, no change");
        else if(err == NEO_NEW_SUCCESS)  // new sequence started
            rest_response_setGo(ESP_OK, "sequence change successful");
        else if(err < NEO_SUCCESS)  {
            neo_cycle_stop();  // error, stop
            rest_response_setGo(ESP_ERR_NOT_SUPPORTED, "error processing button");
        }
    }
}

/*
 * top level process to manage servo communication
 * SAMPLE/TEST FUNCTION FOR NOW
 */
static void servo_process(void *pvParameters)  {
   ESP_LOGI(TAG, "Initializing servo subsystem...");
   if(servo_init() != ESP_OK)
        ESP_LOGE(TAG, "Error initializing servos");

   int32_t angle = 0;
   uint8_t ch = 0;
    /*
     * +/- 45 test
     */
    while(1)  {
            //servo_move_real_pre(ch, servo_defs[ch].mina, false);  // absolute move to ccw
      servo_rest(ch);  // back to middle
      ESP_LOGI(TAG, "top rest move resulted in %ld deg", servo_get_angle(ch));

      vTaskDelay(1000 / portTICK_PERIOD_MS);

      /*
       * make 45 +1 deg relative moves
       */
      ESP_LOGI(TAG, "make 45 +1 moves...");
      for(int i = 0; i < 45; i++)  {
        servo_move_real_pre(ch, 1, true);
        vTaskDelay(10 / portTICK_PERIOD_MS);
      }
      ESP_LOGI(TAG, "at end if 45 +1 moves %ld deg", servo_get_angle(ch));
      vTaskDelay(1000 / portTICK_PERIOD_MS);

      servo_rest(ch);  // back to middle
      vTaskDelay(1000 / portTICK_PERIOD_MS);

            /*
       * make 45 +1 deg relative moves
       */
      ESP_LOGI(TAG, "make 45 -1 moves...");
      for(int i = 0; i < 45; i++)  {
        servo_move_real_pre(ch, -1, true);
        vTaskDelay(10 / portTICK_PERIOD_MS);
      }
      ESP_LOGI(TAG, "at end if 45 -1 moves %ld deg", servo_get_angle(ch));
      vTaskDelay(1000 / portTICK_PERIOD_MS);

    }
}

/*
 * start the wifi station - helper to unclutter main()
 */

void init_wifi(void)  {

    esp_netif_ip_info_t ip;  // potential static ip address

    /*
     * convert the string based fixed ip address parameter
     * to the stacked 32-bit version that the espressif netif requires.
     */
    uint32_t ip32_addr = 0;

    /*
     * if DHCP is disabled, convert the string-based static IP address
     * and set it where the wifi init can see it.
     * 
     * if there is an error converting the IP address, 
     * use the values that are compiled in via menuConfig.
     */
    if(strcmp("false", pmon_config->dhcp_enable) == 0)  {
        // static ip address
        if((ip32_addr = ipaddr_addr(pmon_config->ipaddr)) == IPADDR_NONE)  {
            ESP_LOGE(TAG, "Error converting IP address %s from eeprom", pmon_config->ipaddr);
            ip.ip.addr = ipaddr_addr(CONFIG_EXAMPLE_STATIC_IP_ADDR);
        }
        else  {
            ESP_LOGI(TAG, "DHCP disabled, setting static IP address: %s (0x%lx)", pmon_config->ipaddr, ip32_addr);
            ip.ip.addr = ip32_addr;
        }

        // static gateway address
        if((ip32_addr = ipaddr_addr(pmon_config->gwaddr)) == IPADDR_NONE)  {
            ESP_LOGE(TAG, "Error converting GW address %s from eeprom", pmon_config->gwaddr);
            ip.gw.addr = ipaddr_addr(CONFIG_EXAMPLE_STATIC_GW_ADDR);
        }
        else  {
            ESP_LOGI(TAG, "DHCP disabled, setting static GW address: %s (0x%lx)", pmon_config->gwaddr, ip32_addr);
            ip.gw.addr = ip32_addr;
        }

        // static netmask
        if((ip32_addr = ipaddr_addr(pmon_config->netmask)) == IPADDR_NONE)  {
            ESP_LOGE(TAG, "Error converting Netmask %s from eeprom", pmon_config->netmask);
            ip.netmask.addr = ipaddr_addr(CONFIG_EXAMPLE_STATIC_NETMASK_ADDR);
        }
        else  {
            ESP_LOGI(TAG, "DHCP disabled, setting static Netmask: %s (0x%lx)", pmon_config->netmask, ip32_addr);
            ip.netmask.addr = ip32_addr;
        }
        set_static_ip_address_data(ip);
    }


    if(wifi_init_sta(CONFIG_ESP_WIFI_SSID, CONFIG_ESP_WIFI_PASSWORD, 
                    (strcmp("true", pmon_config->dhcp_enable) ? false : true)) == ESP_OK)
        ESP_LOGI(TAG, "wifi connected successfully");
    else
        ESP_LOGE(TAG, "wifi couldn't connect to %s", CONFIG_ESP_WIFI_SSID);

}


/*

 * 
 */
void app_main(void)
{
    bool out = false;

    esp_log_level_set("*", NEO_DEBUG_LEVEL);

    /*
     * Configuration CLI
     */
    eeprom_begin();  // initialize the NVS blob used for eeprom-like parameter storage
    CLI_PRINTF("%s\n", EEPROM_INTRO_MSG);
    CLI_PRINTF("Press any key to configure ... \n");
    prompt_countdown(&out);  // give the user (n) seconds to change parameters
    eeprom_user_input(out);  // get the user input based on whether the user hit a key
    pmon_config = get_mon_config_ptr();
    
    /*
     * setup wifi networking and mdns
     */
    ESP_LOGI(TAG, "Initializing NVS ...");
    ESP_ERROR_CHECK(nvs_flash_init());

    ESP_LOGI(TAG, "Initializing underlying tcp/ip stack ...");
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_LOGI(TAG, "Starting event loop ...");
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "Initializing mdns ...");
    initialise_mdns();
    netbiosns_init();
    ESP_LOGI(TAG, "Setting hostname to \"%s\" ...", CONFIG_EXAMPLE_MDNS_HOST_NAME);
    netbiosns_set_name(CONFIG_EXAMPLE_MDNS_HOST_NAME);

    ESP_LOGI(TAG, "Initializing wifi ...");
    init_wifi();

    ESP_LOGI(TAG, "Initializing local filesystem ...");
    ESP_ERROR_CHECK(init_fs());

    /*
     * start the webserver
     */
    ESP_LOGI(TAG, "Starting webserver ...");
    ESP_ERROR_CHECK(start_rest_server(CONFIG_EXAMPLE_WEB_MOUNT_POINT));

    /*
     * start the request response handler
     */
    ESP_LOGI(TAG, "Initializing response handling structures and semaphores...");
    rest_init_resp_data();

    /*
     * start the neopixel engine in a separate task
     */
    ESP_LOGI(TAG, "Starting neopixel process from main() ...");
    xTaskCreate(neopixel_process, NEO_TASK_HANDLE_NAME, 4096, NULL, 10, NULL);

    /*
     * start the servo move engine in a separate task
     */
    //ESP_LOGI(TAG, "Starting servo process from main() ...");
    //xTaskCreate(servo_process, SERVO_TASK_HANDLE_NAME, 4096, NULL, 10, NULL);
}
