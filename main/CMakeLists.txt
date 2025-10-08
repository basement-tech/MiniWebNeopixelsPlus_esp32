idf_component_register(SRCS "neo_script.c" "neo_parsing.c" "rainbow.c" "station_example.c" "esp_rest_main.c"
                            "rest_server.c"
                            "bt_eepromlib.c"
                            "led_strip_encoder.c"
                            "neo_ll_api.c"
                            "neo_play.c"
                            "neo_data.c"
                            "station_example.c"
                            "servo_defs.c"
                            "pca9685.c"
                    INCLUDE_DIRS ".")

#if(CONFIG_EXAMPLE_WEB_DEPLOY_SF)
#    set(WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../front/web-demo")
#    if(EXISTS ${WEB_SRC_DIR}/dist)
#        spiffs_create_partition_image(www ${WEB_SRC_DIR}/dist FLASH_IN_PROJECT)
#    else()
#        message(FATAL_ERROR "${WEB_SRC_DIR}/dist doesn't exit. Please run 'npm run build' in ${WEB_SRC_DIR}")
#    endif()
#endif()
