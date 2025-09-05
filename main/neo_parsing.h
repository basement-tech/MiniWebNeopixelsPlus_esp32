#ifndef __NEO_PARSING__H__
#define __NEO_PARSING__H__

#include <stdint.h>


bool data_valid_OG(void *user);
int8_t neo_proc_OG(char *buf, int json_len, int binsize);

bool data_valid_BIN_BW(void *pbin_len);
int8_t neo_proc_BIN_BW(char *buf, int json_len, int binsize);


#endif // __NEO_PARSING__H__