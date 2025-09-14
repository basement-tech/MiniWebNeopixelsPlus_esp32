#ifndef __SERVO_DEFS_H__
#define __SERVO_DEFS_H__

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t channel;  // channel in controller
    uint16_t servo_min; // pca9685 value corresponding to the most ccw angle
    uint16_t servo_mid; // pca9685 value corresponding to midpoint (save calculations)
    uint16_t servo_max; // pca9685 value corresponding to the largest cw angle
    int32_t mina;  // realworld angle corresponding to servo_min
    int32_t maxa;  // realworld angle corresponding to servo_max
    int32_t cura;  // last successfully set angle
    float pre_incr;  // precalculated mapping increment per degree
    bool bound_sign;  // which is greater maxa or mina (for bound checking)
}  servo_def_t;

/*
 * futaba 3107
 */
#define FUTABA_3107_MINA -45
#define FUTABA_3107_MAXA 45

/*
 * controller options
 */
#define SERVO_CTL_ADAFRUIT_PCA9685_I2C 1
#define SERVO_CTL_GPIO 2  // bit banging
#define SERVO_CONTROLLER SERVO_CTL_ADAFRUIT_PCA9685_I2C


/*
 * servo API
 */
extern servo_def_t servo_defs[];
esp_err_t servo_init(void);
esp_err_t servo_move_real_pre(uint8_t ch, int32_t rangle, bool relative, int32_t *aangle);
esp_err_t servo_rest(uint8_t ch);
int32_t servo_get_angle(uint8_t ch);
int32_t servo_get_numservos(void);

#endif //__SERVO_DEFS_H__
