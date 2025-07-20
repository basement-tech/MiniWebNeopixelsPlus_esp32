/*
 * a simple example to drive the PCA9685, i2c 16-channel PWM chip.
 *
 * this was based on an example from the internet.  Don't know whether
 * it was AI generated, but it didn't build and had some mistakes
 * and missing characters, which were corrected using the datasheet.
 * 
 * NOTE: Ported to new i2c driver ... see breadcrumbs throughout
 * 
 */

#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_types.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "pca9685.h"

static const char *TAG = "PCA9685_SERVO";

/*
 * i2c configuration that works with the pca9685
 * 16 channel pwm controller
 */
i2c_master_bus_handle_t bus_handle;

i2c_device_config_t dev_cfg = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address = PCA9685_I2C_ADDRESS,
    .scl_speed_hz = I2C_MASTER_FREQ_HZ,
};
i2c_master_dev_handle_t dev_handle;

i2c_master_bus_config_t i2c_mst_config = {
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .i2c_port = I2C_MASTER_NUM,
    .scl_io_num = I2C_MASTER_SCL_IO,
    .sda_io_num = I2C_MASTER_SDA_IO,
    .flags.enable_internal_pullup = true,
    .glitch_ignore_cnt = 7,
};

/*
 * initialize the i2c bus according to the spec above
 */
esp_err_t i2c_master_init(void)  {
    esp_err_t err = ESP_OK;

    // set the config in bus_handle structure
    if((err = i2c_new_master_bus(&i2c_mst_config, &bus_handle)) != ESP_OK)  
        ESP_LOGE(TAG, "Error setting i2c master bus ... i2c will not funcion");

    // configure the device characteristics
    else if((err = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle)) != ESP_OK)
        ESP_LOGE(TAG, "Error adding i2c master bus ... i2c will not function");

    return(err);  // closest

}


esp_err_t pca9685_write_byte(uint8_t reg, uint8_t data) {
    esp_err_t err = ESP_OK;
    uint8_t write_buf[2] = {reg, data};

    if((err = i2c_master_transmit(dev_handle, write_buf, sizeof(write_buf), 1000 / portTICK_PERIOD_MS)) != ESP_OK)
        ESP_LOGE(TAG, "error wring to i2c bus");

    return(err);
}

/*
 * each pwm channel has four 8-bit registers in address order.
 * channel 0 starts at the address 0x06, hence the math below
 * for register addresses
 * 
 * From the datasheet:
 * The turn-on time of each LED driver output and the duty cycle of PWM can be controlled
 * independently using the LEDn_ON and LEDn_OFF registers.
 * There will be two 12-bit registers per LED output. These registers will be programmed by
 * the user. Both registers will hold a value from 0 to 4095. One 12-bit register will hold a
 * value for the ON time and the other 12-bit register will hold the value for the OFF time. The
 * ON and OFF times are compared with the value of a 12-bit counter that will be running
 * continuously from 0000h to 0FFFh (0 to 4095 decimal).
 * Update on ACK requires all 4 PWM channel registers to be loaded before outputs will
 * change on the last ACK.
 * The ON time, which is programmable, will be the time the LED output will be asserted and
 * the OFF time, which is also programmable, will be the time when the LED output will be
 * negated. In this way, the phase shift becomes completely programmable. The resolution
 * for the phase shift is 1⁄4096 of the target frequency. Table 7 (datasheet) lists these registers.
 */
esp_err_t pca9685_set_pwm(uint8_t channel, uint16_t on, uint16_t off) {
    uint8_t write_buf[5] = {
        0x06 + 4 * channel,  // LEDn_ON_L register address
        on & 0xFF,           // Low byte of ON value
        on >> 8,             // High byte of ON value
        off & 0xFF,          // Low byte of OFF value
        off >> 8             // High byte of OFF value
    };
    return(i2c_master_transmit(dev_handle, write_buf, sizeof(write_buf), 1000 / portTICK_PERIOD_MS));
}

esp_err_t pca9685_init(void) {
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "Initializing PCA9685...");
    if((err = i2c_master_init()) != ESP_OK)
        ESP_LOGE(TAG, "error initializing i2c for pca9685");
    else  {
        // Set PWM frequency to 50 Hz
        uint8_t prescale = (uint8_t)(round((float)FREQUENCY_OSCILLATOR / (PCA9685_BIT_RESOLUTION * PCA9685_SERVO_FREQ))) - 1;
        ESP_LOGI(TAG, "prescale = %u", prescale);

        // Put PCA9685 into sleep mode
        if((err = pca9685_write_byte(PCA9685_MODE1, MODE1_SLEEP)) != ESP_OK) 
            ESP_LOGE(TAG, "error putting pca9685 into initial sleep mode");
        // set the prescale value
        else if((err = pca9685_write_byte(PCA9685_PRESCALE, prescale)) != ESP_OK)
            ESP_LOGE(TAG, "error setting pca9685 prescale value");
        // Restart and set to normal mode
        else if((err = pca9685_write_byte(PCA9685_MODE1, MODE1_RSTN)) != ESP_OK)
            ESP_LOGE(TAG, "error returning pca9685 to normal mode");
    }
    return(err);
}


