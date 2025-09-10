/*
 * servo_defs.c
 *
 * abstract the math interface to the physical servos
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "esp_log.h"
#include "esp_err.h"
#include "bt_eepromlib.h"
#include "pca9685.h"
#include "servo_defs.h"

#define TAG "servo"

/*
 * define the servos
 *  ch  min pulse width     mid pulse width    max pulse width    min angle(deg)    max angle(deg)  cur angle  pre_incr
 *  --  ---------------     ---------------    ---------------    --------------    --------------  ---------  --------
 */
servo_def_t servo_defs[PCA9685_MAX_CHANNELS] = {
   {0,  PCA9685_SERVO_MIN, PCA9685_SERVO_MID, PCA9685_SERVO_MAX, FUTABA_3107_MINA, FUTABA_3107_MAXA,  0,        0, false},  // Futaba 3107
   {1,  PCA9685_SERVO_MIN, PCA9685_SERVO_MID, PCA9685_SERVO_MAX, FUTABA_3107_MINA, FUTABA_3107_MAXA,  0,        0, false},  // Futaba 3107
   {2,  PCA9685_SERVO_MIN, PCA9685_SERVO_MID, PCA9685_SERVO_MAX, FUTABA_3107_MINA, FUTABA_3107_MAXA,  0,        0, false},  // Futaba 3107
   {3,  PCA9685_SERVO_MIN, PCA9685_SERVO_MID, PCA9685_SERVO_MAX, FUTABA_3107_MINA, FUTABA_3107_MAXA,  0,        0, false},  // Futaba 3107
   {4,  PCA9685_SERVO_MIN, PCA9685_SERVO_MID, PCA9685_SERVO_MAX, FUTABA_3107_MINA, FUTABA_3107_MAXA,  0,        0, false},  // Futaba 3107
   {5,  PCA9685_SERVO_MIN, PCA9685_SERVO_MID, PCA9685_SERVO_MAX, FUTABA_3107_MINA, FUTABA_3107_MAXA,  0,        0, false},  // Futaba 3107
   {6,  PCA9685_SERVO_MIN, PCA9685_SERVO_MID, PCA9685_SERVO_MAX, FUTABA_3107_MINA, FUTABA_3107_MAXA,  0,        0, false},  // Futaba 3107
   {7,  PCA9685_SERVO_MIN, PCA9685_SERVO_MID, PCA9685_SERVO_MAX, FUTABA_3107_MINA, FUTABA_3107_MAXA,  0,        0, false},  // Futaba 3107
   {8,  PCA9685_SERVO_MIN, PCA9685_SERVO_MID, PCA9685_SERVO_MAX, FUTABA_3107_MINA, FUTABA_3107_MAXA,  0,        0, false},  // Futaba 3107
   {9,  PCA9685_SERVO_MIN, PCA9685_SERVO_MID, PCA9685_SERVO_MAX, FUTABA_3107_MINA, FUTABA_3107_MAXA,  0,        0, false},  // Futaba 3107
   {10, PCA9685_SERVO_MIN, PCA9685_SERVO_MID, PCA9685_SERVO_MAX, FUTABA_3107_MINA, FUTABA_3107_MAXA,  0,        0, false},  // Futaba 3107
   {11, PCA9685_SERVO_MIN, PCA9685_SERVO_MID, PCA9685_SERVO_MAX, FUTABA_3107_MINA, FUTABA_3107_MAXA,  0,        0, false},  // Futaba 3107
   {12, PCA9685_SERVO_MIN, PCA9685_SERVO_MID, PCA9685_SERVO_MAX, FUTABA_3107_MINA, FUTABA_3107_MAXA,  0,        0, false},  // Futaba 3107
   {13, PCA9685_SERVO_MIN, PCA9685_SERVO_MID, PCA9685_SERVO_MAX, FUTABA_3107_MINA, FUTABA_3107_MAXA,  0,        0, false},  // Futaba 3107
   {14, PCA9685_SERVO_MIN, PCA9685_SERVO_MID, PCA9685_SERVO_MAX, FUTABA_3107_MINA, FUTABA_3107_MAXA,  0,        0, false},  // Futaba 3107
   {15, PCA9685_SERVO_MIN, PCA9685_SERVO_MID, PCA9685_SERVO_MAX, FUTABA_3107_MINA, FUTABA_3107_MAXA,  0,        0, false}   // Futaba 3107
};

/*
 * map the real world coordinates to the pulse width calibration values
 * and move the servo to the requested real world angle
 * 
 * - pulse width numbers are always positive
 * - real world coordinates can be negative or positive (mina is less than maxa)
 *   tested with -45, 45 and 0, 90  (45, -45 does not work)
 * - all math is integer
 * 
 */
float calc_map_span(uint16_t imin, uint16_t imax, int32_t rmin, int32_t rmax)  {
    int32_t ispan = imax - imin;  // the span of the "to" range
    int32_t  rspan = rmax - rmin;  // the span of the "from" range
    return(ispan/(float)rspan);
}

/*
 * calculate the pulse width from the angle
 * using the values from the indicated servo channel/servo
 */
uint16_t calc_pulse(uint8_t ch, int32_t angle)  {
     uint16_t pulse = servo_defs[ch].servo_min + (angle-servo_defs[ch].mina) * servo_defs[ch].pre_incr;
     return(pulse);
}

int32_t calc_angle(uint8_t ch, uint16_t pulse)  {
    int32_t angle = servo_defs[ch].mina + (pulse - servo_defs[ch].servo_min)/servo_defs[ch].pre_incr;
    return(angle);
}

/*** not tested
uint16_t int_map(int32_t in, uint16_t imin, uint16_t imax, int32_t rmin, int32_t rmax)  {

    uint16_t out = imin + abs(in) * calc_map_span(imin, imax, rmin, rmax);

    return(out);
}
*/

/*
 * precalculate the pca9685 pulsewidth incr/deg and store in servo_defs[]
 */
void servo_precalc(void)  {
    for(int i = 0; i < PCA9685_MAX_CHANNELS; i++)  {
        servo_defs[i].pre_incr = calc_map_span(
                                            servo_defs[i].servo_min, servo_defs[i].servo_max,
                                            servo_defs[i].mina, servo_defs[i].maxa
                                            );
        if(servo_defs[i].maxa > servo_defs[i].mina)  servo_defs[i].bound_sign = true;
        else  servo_defs[i].bound_sign = false;

        ESP_LOGD(TAG, "pre_incr = %f", servo_defs[i].pre_incr);
    }
}

static net_config_t *pmon_config;
bool servo_auth = false;
esp_err_t servo_init(void)  {
    /*
     * read the servo move authorization value from the
     * eeprom parameters and set a local bool.  If "true"
     * then movement will happen
     */
    pmon_config = get_mon_config_ptr();
    if(strcmp("true", pmon_config->servo_auth) == 0)
        servo_auth = true;
    else  {
        servo_auth = false;
        ESP_LOGI(TAG, "WARNING: servo moves not authorized in eeprom");
    }

    servo_precalc();
    return(pca9685_init());
}

/*
 * make a move on the specified channel to the real world angle
 * servo_move_real_pre()  uses the precalculated value from servo_defs[]
 * (need to call servo_precalc() before using this function).
 * truncate the value if exceeds limits.
 * return the angle actual value set.
 * 
 * NOTE: if servo movement is not authorized in the eeprom parameters
 * no physical move is accomplished and the return value cannot be 
 * trusted to match the position of the servo.
 * 
 *   angle                      pulse
 *   -----                      -----

 */
int32_t servo_move_real_pre(uint8_t ch, int32_t angle, bool relative)  {

    if(servo_auth == false)
        return(0);

    /*
     * apply the request for a relative move if so
     * (i.e. convert to absolute)
     */
    if(relative == true)
        angle = servo_defs[ch].cura + angle;

    /*
     * make sure the requested angla is in bounds
     * (use the precalculated bound_sign flag)
     */
    if(servo_defs[ch].bound_sign == true)  {
        if((angle + servo_defs[ch].mina) > servo_defs[ch].maxa)
            angle = servo_defs[ch].maxa;
    }
    else  {
        if((angle + servo_defs[ch].maxa) > servo_defs[ch].mina)
            angle = servo_defs[ch].mina;
    }

    uint16_t pulse = calc_pulse(ch, angle);
    ESP_LOGD(TAG, "pulse = %u", pulse);
    if(pca9685_set_pwm(ch, 0, pulse) == ESP_OK)
        servo_defs[ch].cura = angle;
    else
        ESP_LOGE(TAG, "Error writing to servo");

    return(angle);
}

/*
 * move the servo to its neutral position (middle of travel, 1.5 mS).
 * this function uses the pca9685 middle of range index directly.
 * it then back-calculates position in degrees to fill in the current angle
 * value in servo_defs[].
 * NOTE: the real world angle in deg may not be symetrically mapped to the pulse width span.
 * 
 * NOTE: if servo movement is not authorized in the eeprom parameters
 * no physical move is accomplished and the return value cannot be 
 * trusted to match the position of the servo.
 * 
 */
esp_err_t servo_rest(uint8_t ch)  {
    esp_err_t err = ESP_OK;

    if(servo_auth == false)
        return(ESP_FAIL);

    if((err = pca9685_set_pwm(ch, 0, servo_defs[ch].servo_mid)) != ESP_OK)
        ESP_LOGE(TAG, "Error setting channel %u to neutral position", ch);
    else  {
        servo_defs[ch].cura = calc_angle(ch, servo_defs[ch].servo_mid);
    }
    return(err);
}

/*
 * return the real world coordinate angle for the specified channel
 */
int32_t servo_get_angle(uint8_t ch)  {
    return(servo_defs[ch].cura);
}