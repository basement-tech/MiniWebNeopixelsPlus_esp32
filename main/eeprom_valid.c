/*
 * define functions here to validate eeprom parameter input
 */
#include "stdbool.h"
#include "string.h"
#include "lwip/ip_addr.h"

/*
 * test the input string against "true" and "false"
 * return true if match, false otherwise
 */
bool tORf_valid(char *value)  {
    bool ret = false;

    if(strcmp(value, "true") == 0)
        ret = true;
    else if(strcmp(value, "false") == 0)
        ret = true;
    else
        ret = false;

    return(ret);
}

bool isGoodIP4(char *value)  {
    bool ret = false;

    if(ip4addr_aton(value, NULL) == 1)
        ret = true;
    else
        ret = false;
    
    return(ret);
}