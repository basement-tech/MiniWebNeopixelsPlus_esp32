
#ifndef __NEO_LL_API_H__
#define __NEO_LL_API_H__

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"

typedef struct {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t w; // not tested
} pixel_t;
typedef struct {
    uint16_t numpixels;
    pixel_t *pixels;
} neo_strand_t;

#define RMT_LED_STRIP_RESOLUTION_HZ 10000000 // 10MHz resolution, 1 tick = 0.1us (led strip needs a high resolution)
#define RMT_LED_STRIP_GPIO_NUM      18

esp_err_t pixels_clear(void);
esp_err_t pixels_setcount(uint16_t num_pixels);
uint16_t pixels_numPixels(void);
esp_err_t pixels_alloc(void);
esp_err_t pixels_init(void);
esp_err_t pixels_setPixelColorRGB(uint32_t i, uint8_t r, uint8_t g, uint8_t b, uint8_t w);
esp_err_t pixels_setPixelColorS(uint32_t i, pixel_t pixel);
esp_err_t pixels_show(void);
uint32_t pixels_Color(uint8_t r, uint8_t g, uint8_t b);
uint32_t pixels_gamma32(uint32_t color);


#endif  //__NEO_LL_API_H__