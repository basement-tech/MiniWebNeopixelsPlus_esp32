/*
 * bt_eeprom.h
 * ------
 * this section deals with writing the persistent parameters
 * to the EEPROM and reading them on subsequent reboots
 * E.g. WIFI credentials
 */

#ifndef __BT_EEPROM_H__
#define __BT_EEPROM_H__

#include <Arduino.h>

/*
 * this is the size of the EERPOM segment that is accessible
 * by the EEPROM class.
 * this has to be greater than or equal to 
 * the size of the net_config struct below.
 */
 
#define EEPROM_RESERVE 1024

/*
 * this message is displayed when asking for user input
 */
#define EEPROM_INTRO_MSG "neopixel fun by daniel@basementtech and zimtech, LLC"

/*
 * string to match for validation
 * this indicates the version/structure of the EEPROM too.
 * be sure to update this string if you change the 
 * net_config struct below.
 */
#define EEPROM_VALID  "valid_v0.8.1"

/*
 * map of the parameters stored in EEPROM
 * the EEPROM class is smart enough to deal with
 * custom types, so even if things are added here,
 * the rest of the code should work.
 */
struct net_config  {
char valid[32];          // eeprom version validation string
char dhcp_enable[8];     // enable/disable dhcp
char wlan_ssid[64];      // wifi ssid
char wlan_pass[64];      // wifi password
char ipaddr[64];         // fixed ip address if desired/set
char wifitries[8];       // wifi connection attempts
char tz_offset_gmt[64];  // POSIX standard time zone string
char debug_level[4];     // display messages at different levels of detail (-1 to n)
char neocount[8];        // number of neopixels in the strand
char neogamma[8];        // gamma correction or not
char neodefault[16];     // label of the sequence to load at start
char reformat[8];        // reformat fs on startup
};
 
/*
 * function declarations
 */
net_config *get_mon_config_ptr(void);
void eeprom_user_input(bool out);
int getone_eeprom_input(int i);
void getall_eeprom_inputs();
void dispall_eeprom_parms();
bool eeprom_validation(char match[]);
int l_read_string(char *buf, int blen, bool echo);
int8_t eeprom_convert_ip(char *sipaddr, uint8_t octets[]);
void createHTMLfromEEPROM(char *buf, int size);
void saveJsonToEEPROM(JsonDocument jsonDoc);

void eeprom_begin(void);
void eeprom_get(void);
void eeprom_put(void);

#endif
