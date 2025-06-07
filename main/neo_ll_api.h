
#ifndef __NEO_LL_API_H__
#define __NEO_LL_API_H__

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"

typedef struct {
    uint16_t numpixels;
} neo_strand_t;

#define RMT_LED_STRIP_RESOLUTION_HZ 10000000 // 10MHz resolution, 1 tick = 0.1us (led strip needs a high resolution)
#define RMT_LED_STRIP_GPIO_NUM      0

esp_err_t pixels_clear();
esp_err_t pixels_setcount(uint16_t num_pixels);
esp_err_t pixels_setPixelColor(uint32_t i, uint8_t r, uint8_t g, uint8_t b, uint8_t w);
esp_err_t pixels_show();


#endif  //__NEO_LL_API_H__