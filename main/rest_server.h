#ifndef __REST_SERVER_H__
#define __REST_SERVER_H__

#include "esp_log.h"
#include "esp_http_server.h"

/*
 * data struct used to request a response be sent
 * to a web client, usually from a c language handler
 * of html/js fetch
 */
#define MAX_RESP_MSGTXT 32  // max length of response message text
typedef struct {
    int32_t transaction;
    char msgtxt[MAX_RESP_MSGTXT];
    esp_err_t err;
} rest_resp_queue_t;

void rest_init_resp_data(void);
void rest_response_setGo(esp_err_t err, char *msgtxt);


#endif