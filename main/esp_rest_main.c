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
 * neopixel process
 */
SemaphoreHandle_t xneoMutex;  // used to protect communication to neo_play
neo_mutex_data_t neo_mutex_data;  // data to be sent to neo_play process from webserver

#define NEO_TAG "neopixel_process"
static void neopixel_process(void *pvParameters)  {
    uint16_t count = atoi(pmon_config->neocount);
    bool on = false;
    uint8_t r, g, b;

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
        xSemaphoreGive(xneoMutex);
    }

    /*
     * TODO
     * temporary test of file loading

    if(neo_load_sequence("neo_user_1.json") != 0)
        ESP_LOGE(NEO_TAG, "Error loading test sequence file");
     */
    
    pixels_init();
    pixels_setcount(count);
    ESP_LOGI(NEO_TAG, "Allocating array for %d pixels", count);
    pixels_alloc();
    neo_init();

    while(1)  {
        if(neo_cycle_next_flag == true)  {
            neo_cycle_next_flag = false;
            neo_cycle_next();
        }
        if(seq_upd_flag == true)  {
            seq_upd_flag = false;
            neo_new_sequence();
        }
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

}
