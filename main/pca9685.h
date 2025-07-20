/*
 * support for the pca9685 driver
 * credit to adafruit for typing this all up
 */

#ifndef __PCA9685_H__
#define __PCA9685_H__

#include "esp_err.h"

// REGISTER ADDRESSES
#define PCA9685_MODE1 0x00      /**< Mode Register 1 */
#define PCA9685_MODE2 0x01      /**< Mode Register 2 */
#define PCA9685_SUBADR1 0x02    /**< I2C-bus subaddress 1 */
#define PCA9685_SUBADR2 0x03    /**< I2C-bus subaddress 2 */
#define PCA9685_SUBADR3 0x04    /**< I2C-bus subaddress 3 */
#define PCA9685_ALLCALLADR 0x05 /**< LED All Call I2C-bus address */
#define PCA9685_LED0_ON_L 0x06  /**< LED0 on tick, low byte*/
#define PCA9685_LED0_ON_H 0x07  /**< LED0 on tick, high byte*/
#define PCA9685_LED0_OFF_L 0x08 /**< LED0 off tick, low byte */
#define PCA9685_LED0_OFF_H 0x09 /**< LED0 off tick, high byte */
// etc all 16:  LED15_OFF_H 0x45
#define PCA9685_ALLLED_ON_L 0xFA  /**< load all the LEDn_ON registers, low */
#define PCA9685_ALLLED_ON_H 0xFB  /**< load all the LEDn_ON registers, high */
#define PCA9685_ALLLED_OFF_L 0xFC /**< load all the LEDn_OFF registers, low */
#define PCA9685_ALLLED_OFF_H 0xFD /**< load all the LEDn_OFF registers,high */
#define PCA9685_PRESCALE 0xFE     /**< Prescaler for PWM output frequency */
#define PCA9685_TESTMODE 0xFF     /**< defines the test mode to be entered */

// MODE1 bits
#define MODE1_ALLCAL 0x01  /**< respond to LED All Call I2C-bus address */
#define MODE1_SUB3 0x02    /**< respond to I2C-bus subaddress 3 */
#define MODE1_SUB2 0x04    /**< respond to I2C-bus subaddress 2 */
#define MODE1_SUB1 0x08    /**< respond to I2C-bus subaddress 1 */
#define MODE1_SLEEP 0x10   /**< Low power mode. Oscillator off */
#define MODE1_RSTN 0xA0    /**< reset and set to normal mode */
#define MODE1_AI 0x20      /**< Auto-Increment enabled */
#define MODE1_EXTCLK 0x40  /**< Use EXTCLK pin clock */
#define MODE1_RESTART 0x80 /**< Restart enabled */
// MODE2 bits
#define MODE2_OUTNE_0 0x01 /**< Active LOW output enable input */
#define MODE2_OUTNE_1                                                          \
  0x02 /**< Active LOW output enable input - high impedience */
#define MODE2_OUTDRV 0x04 /**< totem pole structure vs open-drain */
#define MODE2_OCH 0x08    /**< Outputs change on ACK vs STOP */
#define MODE2_INVRT 0x10  /**< Output logic state inverted */

#define PCA9685_I2C_ADDRESS 0x40      /**< Default PCA9685 I2C Slave Address */
#define FREQUENCY_OSCILLATOR 25000000 /**< Int. osc. frequency in datasheet */
#define PCA9685_BIT_RESOLUTION   4096

#define PCA9685_PRESCALE_MIN 3   /**< minimum prescale value */
#define PCA9685_PRESCALE_MAX 255 /**< maximum prescale value */

// PCA9685 I2C address and parameters
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_SDA_IO 23
#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_FREQ_HZ 100000

/*
 * adafruit 16 channel pwm controller
 * NOTE: PCA9685_MAX_CHANNELS controls the size servo_def[]
 * (i.e. can be set to the number of channels used)
 */
#define PCA9685_MAX_CHANNELS 16      // number of channels in the controller
#define PCA9685_SERVO_FREQ   46.5    // operating frequency of the servos yields exactly 20 mS period
#define PCA9685_SERVO_MIN    205     // yields 1.0 mS 
#define PCA9685_SERVO_MID    307     // yields 1.5 mS
#define PCA9685_SERVO_MAX    410     // yields 2.0 mS

/*
 * exposed functions
 */
esp_err_t pca9685_init(void);
esp_err_t pca9685_set_pwm(uint8_t channel, uint16_t on, uint16_t off);


#endif // __PCA9685_H__