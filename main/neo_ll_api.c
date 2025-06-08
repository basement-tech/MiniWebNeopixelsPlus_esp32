#include "neo_ll_api.h"

#define TAG "neo_ll_api.c"

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

esp_err_t pixels_setcount(uint16_t num_pixels)  {
    strand.numpixels = num_pixels;

    return(ESP_OK);
}

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

esp_err_t pixels_setPixelColor(uint32_t i, uint8_t r, uint8_t g, uint8_t b, uint8_t w)  {
    if(strand.pixels == NULL)
        return(ESP_ERR_NO_MEM);
    else {
        strand.pixels[i].r = r;
        strand.pixels[i].g = g;
        strand.pixels[i].b = b;
    }
    return(ESP_OK);
}


esp_err_t pixels_show(void)  {
    //memset(led_strip_pixels, 0, sizeof(led_strip_pixels));
    ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, (const void *)strand.pixels, (sizeof(pixel_t) * strand.numpixels), &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));

    return(ESP_OK);
}
