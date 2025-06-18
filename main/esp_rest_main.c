/****
 * miniwebneopixelsplus
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
 * Trying to preserve the use of esp-idf's Menuconfig functionality.
 * i.e all default configuration is done through the interface in esp-idf.
 * 
 * Components/Libraries
 * --------------------
 * cJSON - used to serialize strings to json
 * espressif_json_parser - deserialize/parse json input
 * espressif_jsmn - espressif_json_parser is built on this
 * joltwallet_littlefs - littleFS embedded filesystem
 * espressif_mdns - mdns for hostname access via .local
 * 
 * Timers
 * ------
 * https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/gptimer.html
 * https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/esp_timer.html
 * 
 * Todo:
 * o Implement multifile upload
 * 
 * 
 *
 * djz 2025
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
#include "protocol_examples_common.h"

#include "neo_system.h"
#include "bt_eepromlib.h"
#include "neo_ll_api.h"
#include "neo_data.h"


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
SemaphoreHandle_t xseq_upd_flag;  // new sequence requested

#define NEO_TAG "neopixel_process"
static void neopixel_process(void *pvParameters)  {
    uint16_t count = atoi(pmon_config->neocount);
    bool on = false;
    uint8_t r, g, b;

    gpio_init();  // for debugging

    if((xneo_cycle_next_flag = xSemaphoreCreateBinary()) == NULL)
        ESP_LOGE(NEO_TAG, "Error creating xneo_cycle_next_flag semaphore");
    else  {
        ESP_LOGI(NEO_TAG, "xneo_cycle_next_flag semaphore created successfully");
        xSemaphoreGive(xneo_cycle_next_flag);  // make it available
    }

    if((xseq_upd_flag = xSemaphoreCreateBinary()) == NULL)
        ESP_LOGE(NEO_TAG, "Error creating xseq_upd_flag semaphore");
    else  {
        ESP_LOGI(NEO_TAG, "xseq_upd_flag semaphore created successfully");
        xSemaphoreGive(xneo_cycle_next_flag);  // make it available
    }

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
    
    pixels_init();
    pixels_setcount(count);
    ESP_LOGI(NEO_TAG, "Allocating array for %d pixels", count);
    pixels_alloc();
    neo_init();

    /*
     * kick-off the first sequence
     */
    if(xSemaphoreTake(xneoMutex, 10/portTICK_PERIOD_MS) == pdFALSE)
        ESP_LOGE(NEO_TAG, "Failed to take mutex on initial sequence set ... no change");
    else  {
        neo_mutex_data.new_data = true;
        xSemaphoreGive(xneoMutex);
    }

    while(1)  {
        /*
         * wait at most 200 mS for the cycle next flag.  After the timeout,
         * check to see if a new sequence was requested.  If a sequence is 
         * running, this might be very often.  If not, it will be at the timeout
         * interval.
         */
        xSemaphoreTake(xneo_cycle_next_flag, NEO_CHK_NEWS_INTERVAL);  // wait for the signal from timer
        gpio_set_level(GPIO_OUTPUT_IO_1, 1);
        neo_cycle_next();
        gpio_set_level(GPIO_OUTPUT_IO_1, 0);

        /*
         * check to see of a new sequence was requested
         */
        neo_new_sequence();
    }

    /*
     * TODO
     * temporary test of neo_pixel activity

    while(1)  {

        if(on == true)  {
            on = false;
            r = 50;
            g = 0;
            b = 0;
        }
        else  {
            on = true;
            r = 0;
            g = 0;
            b = 50;
        }
        for(uint16_t i = 0; i < count; i++)
            pixels_setPixelColorRGB(i, r, g, b, 0);
        pixels_show();
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
*/

}

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
    ESP_ERROR_CHECK(example_connect());

    ESP_LOGI(TAG, "Initializing local filesystem ...");
    ESP_ERROR_CHECK(init_fs());

    
    ESP_LOGI(TAG, "Starting webserver ...");
    ESP_ERROR_CHECK(start_rest_server(CONFIG_EXAMPLE_WEB_MOUNT_POINT));

    /*
     * create a task to play out the neopixel example
     */
    ESP_LOGI(TAG, "Starting neopixel process from main() ...");
    xTaskCreate(neopixel_process, "neopixel_process", 4096, NULL, 10, NULL);
    //xTaskCreatePinnedToCore(neopixel_process, "neopixel_process", 4096, NULL, 10, NULL, 1);

}
