
#include <string.h>

#include "neo_ll_api.h"

#define TAG "neo_ll_api.c"

/*
 * This section sets up the RMT channel to play out neo_pixel
 * data without any software interaction once loaded (hardware driven).
 * NOTE: this is specific to the esp32 and is not available in esp8266.
 * 
 * No space is allocated for the local copy of neo_pixel color data.  That
 * is done separately by pixels_alloc() based on the configured number of pixels
 */
static rmt_channel_handle_t led_chan = NULL;
rmt_tx_channel_config_t tx_chan_config = {
    .clk_src = RMT_CLK_SRC_DEFAULT, // select source clock
    .gpio_num = RMT_LED_STRIP_GPIO_NUM,
    .mem_block_symbols = 64, // increase the block size can make the LED less flickering
    .resolution_hz = RMT_LED_STRIP_RESOLUTION_HZ,
    .trans_queue_depth = 4, // set the number of transactions that can be pending in the background
};
rmt_encoder_handle_t led_encoder = NULL;
led_strip_encoder_config_t encoder_config = {
    .resolution = RMT_LED_STRIP_RESOLUTION_HZ,
};
rmt_transmit_config_t tx_config = {
    .loop_count = 0, // no transfer loop
};

static neo_strand_t strand;  // pointer to strand to be allocated later

esp_err_t pixels_init(void)  {
    ESP_LOGI(TAG, "Create RMT TX channel");
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));

    ESP_LOGI(TAG, "Install led strip encoder");
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));

    ESP_LOGI(TAG, "Enable RMT TX channel");
    ESP_ERROR_CHECK(rmt_enable(led_chan));

    strand.numpixels = 0;
    strand.pixels = NULL;

    return(ESP_OK);
}

/*
 * set the number of pixels in the strand control structure.
 * this is the place that matters for actual playout.
 */
esp_err_t pixels_setcount(uint16_t num_pixels)  {
    strand.numpixels = num_pixels;

    return(ESP_OK);
}

uint16_t pixels_numPixels(void)  {
    return(strand.numpixels);
}

/*
 * allocate the space for the local/working copy (versus the RMT hardware copy)
 * of the neo_pixel color data.  Manipulating this copy has no effect on plaout
 * until pixels_show() is called.
 */
esp_err_t pixels_alloc(void)  {
    /*
     * if this is a reallocation, free first
     */
    if(strand.pixels != NULL)  {
        free(strand.pixels);
    }

    if((strand.pixels = malloc(sizeof(pixel_t) * strand.numpixels)) == NULL)
        return(ESP_ERR_NO_MEM);
    return(ESP_OK);
}

/*
 * set RGB neo_pixel color data in the local copy of such.
 * Manipulating this copy has no effect on plaout
 * until pixels_show() is called.
 */
esp_err_t pixels_setPixelColorRGB(uint32_t i, uint8_t r, uint8_t g, uint8_t b, uint8_t w)  {
    if(strand.pixels == NULL)
        return(ESP_ERR_NO_MEM);
    else {
        strand.pixels[i].r = r;
        strand.pixels[i].g = g;
        strand.pixels[i].b = b;
        strand.pixels[i].w = w;
    }
    return(ESP_OK);
}

/*
 * same as pixels_setPixelColor() except accepts pixel_t as color spec
 */
esp_err_t pixels_setPixelColorS(uint32_t i, pixel_t pixel)  {
    if(strand.pixels == NULL)
        return(ESP_ERR_NO_MEM);
    else {
        memcpy(&(strand.pixels[i]), &pixel, sizeof(pixel));
    }
    return(ESP_OK);
}

/*
 * set all pixels to black in the local copy of neo_pixel color memory
 * (have to pixels_show() to physically realize it)
 */
esp_err_t pixels_clear(void)  {
    for(int i = 0; i < strand.numpixels; i++)  {
        strand.pixels[i].r = 0;
        strand.pixels[i].g = 0;
        strand.pixels[i].b = 0;
        strand.pixels[i].w = 0;
    }
    return(ESP_OK);
}

/*
 * move the local copy of neo_pixel color data to the RMT hardware
 * effecting its playout
 */
esp_err_t pixels_show(void)  {
    ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, (const void *)(strand.pixels), (sizeof(pixel_t) * strand.numpixels), &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));

    return(ESP_OK);
}
