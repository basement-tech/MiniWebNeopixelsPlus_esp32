/*
 * EEPROM
 * ------
 * this section deals with writing the persistent parameters
 * to the EEPROM and reading them on subsequent reboots
 * E.g. WIFI credentials
 *
 * mon_config[] holds the working copies of the eeprom contents.
 * values are maintained in both mon_config[] and the eeprom as 
 * character strings.  They are expected to be converted on use
 * by the code using them based upon local context.
 *
 * struct eeprom_in eeprom_inputs[] provides the list of values that
 * the user is prompted for, using get_all_eeprom_inputs().  It also provides
 * the strings necessary to create individual prompt messages.
 *
 * a version validation string is kept in EEPROM_VALID in sketch main file.
 * management of that string is currently done by main code.  Perhaps it should be moved.
 *
 * eeprom_begin() is expected to be called in setup().
 * eeprom_get() copies the contents of the eeprom to mon_config.
 * eeprom_put() writes the contents of the eeprom with mon_config.
 * 
 * The ESP_IDF_NVS tag ports the code from esp8266 (eeprom under arduino ide) to 
 * esp32(NVS).
 *
 * Copied this from a previous project and modified it to work with this one - DJZ
 *
 */


#include <stdlib.h>  // for atoi()
#include <string.h>  // for strncpy()
#include <ctype.h>  // for isdigit()
#include "esp_log.h"
#include "driver/uart.h"
#include "hal/uart_ll.h"
#include "soc/uart_struct.h"
#include "bt_eepromlib.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "neo_system.h"

static nvs_handle_t eeprom_nvs_handle;  // points to the open nvs partition
#define STORAGE_NAMESPACE "nvs_as_eeprom"
#define EEPROM_BLOB_NAME "app_settings"

#define TAG "bt_eepromlib.c"  //for logging

/*
 * place to hold the settings for network, mqtt, calibration, etc.
 * (i.e. holds the working copy of parameter values)
 */
static net_config_t mon_config;

/*
 * this section deals with getting the user input to
 * potentially change the eeprom parameter values
 * (e.g. change the WIFI credentials)
 * NOTE: cannot be merged with the net_config structure because
 * the structure determines the byte-for-byte contents of the eeprom data.
 * 
 * esp32 porting note: decided to save the data in nvs as one blob and let this
 * structure do as it did with esp8266 eeprom: lay out the byte-for-byte contents
 * of the parameter memory.
 * 
 */
struct eeprom_in  {
  char prompt[64];   // user visible prompt
  char label[32];    // label when echoing contents of eeprom
  char initial[64];  // sized at largest of net_config members
  char *value;       // pointer to the data in net_config (mon_config)
  int  buflen;       // length of size in EEPROM
};

/*
 * NOTE: validation must be at index = 0
 *
 * NOTE: important that the servo auth is false to insure no
 * physical damage when error occurs with eeprom function and
 * attempt is made to limp along with defaults.
 * 
 */
#define EEPROM_ITEMS 15
struct eeprom_in eeprom_input[EEPROM_ITEMS] = {
  {"",                                           "Validation",    "",                                       mon_config.valid,            sizeof(mon_config.valid)},
  {"DHCP Enable (true, false)",                  "WIFI_DHCP",     "false",                                  mon_config.dhcp_enable,      sizeof(mon_config.dhcp_enable)},
  {"Enter WIFI SSID",                            "WIFI_SSID",     "my_ssid",                                mon_config.wlan_ssid,        sizeof(mon_config.wlan_ssid)},
  {"Enter WIFI Password",                        "WIFI_Password", "my_passwd",                              mon_config.wlan_pass,        sizeof(mon_config.wlan_pass)},
  {"Enter Fixed IP Addr",                        "Fixed_IP_Addr", "192.168.1.37",                           mon_config.ipaddr,           sizeof(mon_config.ipaddr)},
  {"Enter GW IP Addr",                           "GW_IP_Addr",    "192.168.1.1",                            mon_config.gwaddr,           sizeof(mon_config.gwaddr)},
  {"Enter Netmask",                              "Netmask",       "255.255.255.1",                          mon_config.netmask,          sizeof(mon_config.netmask)},
  {"WiFi timeout (# of 500 mS tries)",           "WIFI_timeout",  "10",                                     mon_config.wifitries,        sizeof(mon_config.wifitries)},
  {"Enter GMT offset (POSIX string)",            "GMT_offset",    "CST6CDT,M3.2.0/2:00:00,M11.1.0/2:00:00", mon_config.tz_offset_gmt,    sizeof(mon_config.tz_offset_gmt)},
  {"Enter debug level (-1(none) -> 4(verbose))", "debug_level",   "4",                                      mon_config.debug_level,      sizeof(mon_config.debug_level)},
  {"Enter # of neopixels",                       "npixel_cnt",    "24",                                     mon_config.neocount,         sizeof(mon_config.neocount)},
  {"Neopixel gamma (true, false)",               "neo_gamma",     "true",                                   mon_config.neogamma,         sizeof(mon_config.neogamma)},
  {"Enter default seq label (or \"none\")",      "def_neo_seq",   "none",                                   mon_config.neodefault,       sizeof(mon_config.neodefault)},
  {"Reformat FS (true, false)",                  "FS_reformat",   "false",                                  mon_config.reformat,         sizeof(mon_config.reformat)},
  {"Servo move authorized (true, false)",        "Servo auth",    "false",                                  mon_config.servo_auth,       sizeof(mon_config.servo_auth)}
};

/*
 * load the defaults ... skip validation since that determines
 * if this is executed
 */
void set_eeprom_initial(void)  {
  for(int8_t i = 1; i < EEPROM_ITEMS; i++)
    strncpy(eeprom_input[i].value, eeprom_input[i].initial, eeprom_input[i].buflen);
}

void init_eeprom_input()  {
    eeprom_input[0].value = mon_config.valid;
    eeprom_input[1].value = mon_config.dhcp_enable;
    eeprom_input[2].value = mon_config.wlan_ssid;
    eeprom_input[3].value = mon_config.wlan_pass;
    eeprom_input[4].value = mon_config.ipaddr;
    eeprom_input[5].value = mon_config.gwaddr;
    eeprom_input[6].value = mon_config.netmask;
    eeprom_input[7].value = mon_config.wifitries;
    eeprom_input[8].value = mon_config.tz_offset_gmt;
    eeprom_input[9].value = mon_config.debug_level;
    eeprom_input[10].value = mon_config.neocount;
    eeprom_input[11].value = mon_config.neogamma;
    eeprom_input[12].value = mon_config.neodefault;
    eeprom_input[13].value = mon_config.reformat;
    eeprom_input[14].value = mon_config.servo_auth;
}

/*
 * break the data compartmentalization a little and allow the calling
 * data space to access mon_config directly.  I'm hoping this will be read-only.
 */
net_config_t *get_mon_config_ptr(void) {
	return(&mon_config);
}

/*
 * prompt for and set one input in eeprom_input[].value.
 * return: that which comes back from l_read_string()
 */
int getone_eeprom_input(int i)  {
  char inbuf[64];
  int  insize = 0;

  /*
   * if there is no prompt associated with the subject
   * parameter, skip it
   */
  if(eeprom_input[i].prompt[0] != '\0')  {
    CLI_PRINTF("%s", eeprom_input[i].prompt);
    CLI_PRINTF("["); CLI_PRINTF("%s", eeprom_input[i].value); CLI_PRINTF("]");
    CLI_PRINTF("(max "); CLI_PRINTF("%d", eeprom_input[i].buflen - 1); CLI_PRINTF(" chars):");
    if((insize = l_read_string(inbuf, sizeof(inbuf), true)) > 0)  {
      if(insize < (eeprom_input[i].buflen))
        strcpy(eeprom_input[i].value, inbuf);
      else  {
        CLI_PRINTF("\n"); 
        CLI_PRINTF("Error: too many characters; value will be unchanged\n");fflush(stdout);
      }
    }
    CLI_PRINTF("\n");
  }
  return(insize);
}

void getall_eeprom_inputs()  {
  int i;
  int ret;
  
  CLI_PRINTF("\n");    
  CLI_PRINTF("Press <enter> alone to accept previous EEPROM value shown\n");
  CLI_PRINTF("Press <esc> as the first character to skip to the end\n");
  CLI_PRINTF("\n");

  /*
   * loop through getting all of the EEPROM parameter user inputs.
   * if <esc> (indicated by -2) is pressed, skip the balance.
   */
  i = 0;
  ret = 0;
  while((i < EEPROM_ITEMS) && (ret != -2))  {
    ret = getone_eeprom_input(i);
    i++;
  }
}

void dispall_eeprom_parms()  {
  
  CLI_PRINTF("\n");    
  CLI_PRINTF("Local copy of EEPROM contents(");
  CLI_PRINTF("%d", sizeof(mon_config)); printf(" of ");
  CLI_PRINTF("%d", EEPROM_RESERVE); printf(" bytes used):\n");

  /*
   * loop through getting all of the EEPROM parameter user inputs.
   * if <esc> (indicated by -2) is pressed, skip the balance.
   */
  for(int i = 0; i < EEPROM_ITEMS; i++)  {
    CLI_PRINTF("%s", eeprom_input[i].label);
    CLI_PRINTF(" ->"); CLI_PRINTF("%s", eeprom_input[i].value); printf("<-\n");
  }
}



/*
 * OK, I just got tired of trying to figure out the available libraries.
 * This function reads characters from the Serial port until an end-of-line
 * of some sort is encountered.
 * 
 * I used minicom under ubuntu to interact with this function successfully.
 * 
 * buf : is a buffer to which to store the read data
 * blen : is meant to indicate the size of buf
 * echo : whether to echo the characters or not 
 * 
 * Return: (n)  the number of characters read, not counting the end of line,
 *              which is over-written with a string terminator ('\0')
 *         (-1) if the buffer was overflowed
 *         (-2) if the <esc> was entered as the first character
 * 
 */
int l_read_string(char *buf, int blen, bool echo)  {
  int count = 0;
  bool out = false;
  int ret = -1;
  uint32_t len = 0;  // not an _t type since that's the way the function is defined

  /*
   * temporarily turn off logging to allow reading on the monitor port
   */
  esp_log_level_set("*", ESP_LOG_NONE);
  uart_flush_input(UART_NUM_0);

  while((out == false) && (count < blen))  {
    len = uart_ll_get_rxfifo_len(UART_LL_GET_HW(UART_NUM_0));
    if(len > (uint32_t)0)  {
      fflush(stdout);
      uart_ll_read_rxfifo(UART_LL_GET_HW(UART_NUM_0), (uint8_t *)buf, 1);
#ifdef FL_DEBUG_MSG
      printf("char=");printf("%c", *buf);printf("%x\n", *buf);fflush(stdout);
#endif
      /*
       * echo if commanded to do so by the state of the echo argument.
       * don't echo the <esc>.
       */
      if((echo == true) && (*buf != '\x1B'))  {
        printf("%c", *buf);fflush(stdout);
      }
      switch(*buf)  {
        /*
         * terminate the string and get out
         */
        case '\n':
          *buf = '\0';
          out = true;
        break;

        case '\r':
          *buf = '\0';
          out = true;
        break;

        /*
         * <escape> was entered
         * ignored if not the first character
         */
        case '\x1B':
          if(count == 0)  {
            out = true;
            count = -2;
          }
        break;

        /* 
         * backspace: don't increment the buffer pointer which
         * allows this character to be written over by the next
         */
        case '\b':
          if(count > 0)
            buf--;
          count--;
          printf(" \b");fflush(stdout);  /* blank out the character */
        break;

        /*          
         * normal character
         */
        default:
          buf++;
          count++;
        break;
      }
    }  // if input
    vTaskDelay(10/portTICK_PERIOD_MS);
  }
  esp_log_level_set("*", NEO_DEBUG_LEVEL);

  /*
   * compiler wouldn't let me have the return() inside the if()'s
   */
  if(out == true) /* legitimate exit */
    ret = count;
  else if(count == blen)  /* buffer size exceeded */
    ret = -1;
  return(ret);
}


/*
 * "EEPROM" init, read, write
 */

/*
 * if the blob is empty or the initial sizeof(match) does
 * not match the arguments contents, then return false.
 * 
 * compare to see if the eeprom has
 * ever been written with a valid set of data from this
 * exact revision.
 * 
 * returns bool true if an exact match to stored version string
 * 
 */
bool eeprom_validation(char match[])  {
  esp_err_t err = ESP_OK;
  size_t required_size = 0;  // value will default to 0, if not set yet in NVS
  char ebuf[sizeof(mon_config)];
  bool ret = false;

  err = nvs_get_blob(eeprom_nvs_handle, EEPROM_BLOB_NAME, NULL, &required_size);
  if(err != ESP_OK)  {
    ESP_LOGE(TAG, "error reading parameter blob %s", EEPROM_BLOB_NAME);
    ret = false;
  }
  else  {
    ESP_LOGI(TAG, "nvs_get_blob successrully reported size = %d", required_size);
    if(required_size == 0)
      ret = false; // empty; no match
    else  {
      nvs_get_blob(eeprom_nvs_handle, EEPROM_BLOB_NAME, ebuf, &required_size);
      ebuf[strlen(match)] = '\0';
      printf("eeprom validation string to match: >%s<\n", match);fflush(stdout);
      printf("eeprom validation string from memory: >%s<\n", ebuf);fflush(stdout);
      if(strcmp(match, ebuf) == 0)
        ret = true;
      else
        ret = false;
    }
  }
  return(ret);
}

void eeprom_begin(void) {

    size_t required_size = 0;  // value will default to 0, if not set yet in NVS
	  esp_err_t err = nvs_flash_init();

    /*
     * the nvs should never be full.  If the init fails for that reason or because
     * the format seems out of date, "reformat", which is expected to cause defaults
     * to be loaded downstream.
     */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());  // unlikely failure; fatal error
        ESP_ERROR_CHECK(nvs_flash_init());  // unlikely failure; fatal error
    }
    ESP_ERROR_CHECK(nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &eeprom_nvs_handle));

    /*
     * atttempt to get the blob (i.e. confirm that it's there).
     * if it appears not, attempt to create it.
     */
    err = nvs_get_blob(eeprom_nvs_handle, EEPROM_BLOB_NAME, NULL, &required_size);
    if(err != ESP_OK)  {
      ESP_LOGI(TAG, "%s does not exist ... creating", EEPROM_BLOB_NAME);
      nvs_set_blob(eeprom_nvs_handle, EEPROM_BLOB_NAME, (void *)(&mon_config), sizeof(mon_config));  // new mon_config size if new version
    }
}

/*
 * copy the contents of the nvs to the mon_config structure
 */
void eeprom_get(void) {
  size_t required_size;
  nvs_get_blob(eeprom_nvs_handle, EEPROM_BLOB_NAME, (void *)(&mon_config), &required_size);
}

void eeprom_put(void) {
  nvs_set_blob(eeprom_nvs_handle, EEPROM_BLOB_NAME, (void *)(&mon_config), sizeof(mon_config));
	nvs_commit(eeprom_nvs_handle);
}

/*
 * the serial port that is used for the monitor/console port
 * (UART_NUM_0 by default) is configured and initialized by hidden
 * code that is executed prior to calling my app_main() equivalent function.
 * Contrary to what is done with typical user controlled uarts, that is calling
 * driver_install(), this is done at a low level (talks directly to hardware at the
 * HAL level) and therefore doesn't afford the developer access to driver level
 * function call convenience.
 * 
 * Therefore, to do something like the arduino Serial.available(), a low level call
 * to see how many characters are in the input buffer just be called (see below).
 * 
 * this avoids having to put the port in non-blocking mode.
 * 
 */

#define THROW_AWAY_LEN 32
esp_err_t prompt_countdown(bool *out)  {
    int8_t i = CLI_COUNTDOWN_STEPS;
    size_t len = 0;
    uint8_t throw_away = '\0';  // assume that the user doesn't enter more than 32 characters

    *out = false;
    do  {
        len = uart_ll_get_rxfifo_len(UART_LL_GET_HW(UART_NUM_0));
        CLI_PRINTF("%d ... ", i--);
        vTaskDelay(CLI_COUNTDOWN_PERIOD / portTICK_PERIOD_MS);
    }  while((len <= 0) &&  (i > 0));
    ESP_LOGD(TAG, "Throwing away %d bytes\n", len);
    if(len > (size_t)0)  {
        *out = true;
        // Disable UART0 logs for communication
        esp_log_level_set("*", ESP_LOG_NONE);
        while( len-- > (size_t)0)
            uart_ll_read_rxfifo(UART_LL_GET_HW(UART_NUM_0), &throw_away, 1);
        // Re-enable UART0 logging for monitoring
        esp_log_level_set("*", NEO_DEBUG_LEVEL);
    }
    return ESP_OK;
}

/*
 * eeprom_user_input()
 *
 * attempt to validate the eeprom contents.
 * if valid, read the contents into the working copy of parameters.
 * if not valid, load the working copy with default values.
 * if the out flag is set, ask the user to customize the parameter values.
 */
void eeprom_user_input(bool out)  {

  char inbuf[64];
  bool save = false;

  /*
    * if the eeprom contains valid contents from a previously
    * successful user input session, just get it.
    *
    * if not, load the default/compiled in values as initial
    * eeprom values
    *
    * ... proceed to get user input
    */
  if(eeprom_validation((char *)EEPROM_VALID) == true)  {
    eeprom_get();  /* if the EEPROM is valid, get the whole contents */
    CLI_PRINTF("\n");
    dispall_eeprom_parms();
  }
  else  {
    ESP_LOGI(TAG, "Notice: eeprom contents invalid or first time ... loading defaults\n");
    set_eeprom_initial();
  }

  /*
   * if the user entered a character and caused the above
   * while() to exit before the timeout, prompt the user to 
   * enter new network and mqtt configuration data
   * 
   * present previous, valid data from EEPROM as defaults
   */
  if(out == true)  {

    /*
     * run the prompt/input sequenct to get the eeprom changes
     */
    getall_eeprom_inputs();

    CLI_PRINTF("\n");
    dispall_eeprom_parms();
    CLI_PRINTF("Press any key to accept, or reset to correct (no change after 10 sec countdown)\n");
    prompt_countdown(&save);

    if(save == true)  {
      /*
      * if agreed, write the new data to the EEPROM and use it
      */
      if(eeprom_validation((char *)EEPROM_VALID) == true)  {
        CLI_PRINTF("EEPROM: previous data exists ... ");
      }
      else  {
        CLI_PRINTF("EEPROM data never initialized ... ");
      }
        
      CLI_PRINTF("overwrite with new values? ('y' or 'n'):");
      out = false;
      do {
        l_read_string(inbuf, sizeof(inbuf), true);
        if(strcmp(inbuf, "y") == 0)
          out = true;
        else if (strcmp(inbuf, "n") == 0)
          out = true;
        else  {
          CLI_PRINTF("\n");
          CLI_PRINTF("EEPROM data valid ... overwrite with new values? ('y' or 'n'):");
        }
      } while(out == false);
      CLI_PRINTF("\n");

      /*
      * write the data to EEPROM if an affirmative answer was given
      */
      if(strcmp(inbuf, "y") == 0)  {
        ESP_LOGI(TAG, "Writing data to EEPROM ...");
        strcpy(mon_config.valid, EEPROM_VALID);
        eeprom_put();
      }
    } /* entering new data */
    
    /*
    * didn't press a key to change parameters
    */
    if(eeprom_validation((char *)EEPROM_VALID) == true)  {
      eeprom_get();
      ESP_LOGI(TAG, "EEPROM data valid ... using it");
      dispall_eeprom_parms();
    }
    else  {
      CLI_PRINTF("EEPROM data NOT valid ... reset and try enter valid data\n");
    }
  }
}




/*
 * convert a string representation of a v4 IP address to the
 * four-octet version that WIFI classes seem to want.
 * - the input string contents is not changed
 * - input validation:
 *    four conversions required
 *    octet value in range of 0 - 255
 *    only numbers between delimiters allowed
 *    premature end of string
 * arguments: sipaddr - null terminated string representation of ip address
 *            octets[] - values are returned in this array
 *
 * return:  0 if successfully confirmed
 *          -1 if any error is encountered
 */
int lisdigit(char in)  {
  if((in <= '9') && (in >= '0'))
    return(1);
  return(0);
}

/*
 * convert a string based ip4 representation of an ip address to four octets
 *
 * octet[3].octet[2].octet[1].octet[0]
 * 
 * return value:
 * 0  : no errors
 * -1 : range error after conversion
 * -2 : non-numeric digit encountered
 * -3 : temp buffer overflow
 */
int8_t eeprom_convert_ip(char *sipaddr, uint8_t octets[])  {
  int8_t ret = 0;  // start with no errors
  int8_t converts = 0;  // number of dots found as error check
  int32_t value = 0;  // temp in case out of bounds

  char lbuf[32], *plbuf;  //a little big incase a malformed addr is given
  int8_t nbuf = sizeof(lbuf);  // don't go past the end of the buffer; not an index

  plbuf = lbuf;
  for(int i = 0; ((i <= 3) && (ret == 0)); i++)  {
    while((*sipaddr != '.') && (*sipaddr != '\0') && (ret == 0))  {
      if(lisdigit(*sipaddr) == 0)
        ret = -2;
      else if(--nbuf <= 0)
        ret = -3;
      else
        *plbuf++ = *sipaddr++;  // find the next delimiter
    }
    /*
     * normal while() exit; delimiter or terminator found, convert
     */
    if(ret == 0) {
      *plbuf = '\0';  // terminate the octet string buffer
      value = atoi(lbuf);  // convert to integer
      if((value > 255) || (value < 0)) ret = -1;  // range validation
      else {
         octets[i] = value;  // assign to return array
         converts++;
      }
      plbuf = lbuf;  // reset octet string buffer
    }

    /*
     * test to make sure that the end of string
     * happened as it should and/or is the local buffer
     * about to be overrun
     */
    if(*sipaddr == '\0')  {
      if(converts != 4)  // end of string reached before three delimiters
        ret = -1;  // prematurely reached end of string
    }
    else if(--nbuf <= 0)  // like 192.168.1.5555555555555555555555...
      ret = -1;
    else  // success ... continue
      sipaddr++;  // increment past the '.'
    }

  return(ret);
}

uint32_t eeprom_stack_ip(uint8_t octets[])  {
  uint32_t ip32 = 0x0;
  uint32_t shifted_octet = 0x0;

  for(int8_t i = 3, bits = 24; i >= 0; i--, bits -= 8)  {
    shifted_octet = 0x0;
    shifted_octet = (octets[i] & 0xff) << bits;
    ip32 |= shifted_octet;
  }

  return(ip32);
}

/*
 * dynamically create the html form with default values from 
 * the current EEPROM values
 * if overflow occurs, the result is truncated.
 *
 * arguments:
 *  buf -  character buffer to accumulate html/js
 *         expected to have some contents (e.g. js) when passed
 *  size - characters left in the buffer before overflow
 */
void createHTMLfromEEPROM(char *buf, int size)  {
  buf[0] = '\0';

  if(eeprom_validation((char *)EEPROM_VALID) == 0)  {
    eeprom_get();  /* if the EEPROM is valid, get the whole contents */
    init_eeprom_input();
  }
  else  {
    printf("Notice: eeprom contents invalid or first time ... loading defaults\n");
    set_eeprom_initial();
  }

  /*
   * loop through creating the input elements of the html from the eeprom contents
   * start at 1 to skip the validation element
   * initial total string length was 898
   * note that strncpy() attempts to pad the full (n) bytes with '\0' and can overflow
   *
   * WARNING: you are very close to the RAM limit and printf()'s and the like
   * seem to malloc() a large buffer for large strings and cause an exception/reboot.
   */
  int bufsize = size - 1;  // I think I have to save one for the final '\0'

  strncpy((char*)(buf+strlen(buf)), "\t<form onsubmit=\"deviceConfig(event)\">\n",                             (((bufsize-(int)strlen(buf)) < 0) ? 0 : bufsize-strlen(buf)));
  for(int parm = 1; parm < EEPROM_ITEMS; parm++)  {
        strncpy((char*)(buf+strlen(buf)), "\t<label for=\"",                                                   (((bufsize-(int)strlen(buf)) < 0) ? 0 : bufsize-strlen(buf)));
        strncpy((char*)(buf+strlen(buf)), eeprom_input[parm].label,                                            (((bufsize-(int)strlen(buf)) < 0) ? 0 : bufsize-strlen(buf)));
        strncpy((char*)(buf+strlen(buf)), "\">",                                                               (((bufsize-(int)strlen(buf)) < 0) ? 0 : bufsize-strlen(buf)));
        strncpy((char*)(buf+strlen(buf)), eeprom_input[parm].label,                                            (((bufsize-(int)strlen(buf)) < 0) ? 0 : bufsize-strlen(buf)));
        strncpy((char*)(buf+strlen(buf)), " </label>\n",                                                       (((bufsize-(int)strlen(buf)) < 0) ? 0 : bufsize-strlen(buf)));
        strncpy((char*)(buf+strlen(buf)), "\t<input type=\"text\" class=\"config-input-field\" id=\"",         (((bufsize-(int)strlen(buf)) < 0) ? 0 : bufsize-strlen(buf)));
        strncpy((char*)(buf+strlen(buf)), eeprom_input[parm].label,                                            (((bufsize-(int)strlen(buf)) < 0) ? 0 : bufsize-strlen(buf)));
        strncpy((char*)(buf+strlen(buf)), "\" name=\"",                                                        (((bufsize-(int)strlen(buf)) < 0) ? 0 : bufsize-strlen(buf)));
        strncpy((char*)(buf+strlen(buf)), eeprom_input[parm].label,                                            (((bufsize-(int)strlen(buf)) < 0) ? 0 : bufsize-strlen(buf)));
        strncpy((char*)(buf+strlen(buf)), "\" value=\"",                                                       (((bufsize-(int)strlen(buf)) < 0) ? 0 : bufsize-strlen(buf)));
        strncpy((char*)(buf+strlen(buf)), eeprom_input[parm].value,                                            (((bufsize-(int)strlen(buf)) < 0) ? 0 : bufsize-strlen(buf)));
        strncpy((char*)(buf+strlen(buf)), "\"/><br><br>\n",                                                    (((bufsize-(int)strlen(buf)) < 0) ? 0 : bufsize-strlen(buf)));
  }
  strncpy((char*)(buf+strlen(buf)), "\t<button type=\"submit\" class=\"config-button\">Save</button>\n",       (((bufsize-(int)strlen(buf)) < 0) ? 0 : bufsize-strlen(buf)));
  strncpy((char*)(buf+strlen(buf)), "\t<button type=\"button\" class=\"config-button\" onclick=\"handleCancel()\">Reboot</button>\n", 
                                                                                                               (((bufsize-(int)strlen(buf)) < 0) ? 0 : bufsize-strlen(buf)));
  strncpy((char*)(buf+strlen(buf)), "\t</form>\n",                                                             (((bufsize-(int)strlen(buf)) < 0) ? 0 : bufsize-strlen(buf)));
  buf[bufsize] = '\0';  // just in case ... note already reduced by one above

  printf("html buflen=");
  printf("%d", strlen(buf));
  printf("\n");

}

#ifdef NOT_YET
/*
 * copy the result from the browser based input (json),
 * back to the local C structure.
 *
 * arguments:
 *  jsonDoc - jsonDoc containing the json formatted return 
 *    values from the browser based input
 */
void saveJsonToEEPROM(JsonDocument jsonDoc)  {

  for(int parm = 1; parm < EEPROM_ITEMS; parm++)  {
    if(jsonDoc[eeprom_input[parm].label].isNull() == false)  {
      strncpy(eeprom_input[parm].value, jsonDoc[eeprom_input[parm].label], (eeprom_input[parm].buflen-1));
      printf("Saving to eeprom_input[] ");
      printf()"%s", eeprom_input[parm].label);
      printf("=");
      printf("%s\n", eeprom_input[parm].value);
    }
  }

  eeprom_put();  // write to physical eeprom/flash
}
#endif

