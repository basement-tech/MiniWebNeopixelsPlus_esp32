#ifndef __NEO_PARSING__H__
#define __NEO_PARSING__H__

#include <stdint.h>


bool data_valid_OG(void *user);
int8_t neo_proc_OG(char *buf, int json_len, int binsize);
int8_t parse_pts_OG(jparse_ctx_t *pjctx, uint8_t seq_idx, void *user);

int8_t parse_pts_BW(jparse_ctx_t *pjctx, uint8_t seq_idx, void *bin_data);

bool data_valid_BIN_BBW(void *pbin_len);
int8_t neo_proc_BIN_BBW(char *buf, int json_len, int binsize);
int8_t parse_pts_BBW(jparse_ctx_t *pjctx, uint8_t seq_idx, void *bin_data);


#endif // __NEO_PARSING__H__