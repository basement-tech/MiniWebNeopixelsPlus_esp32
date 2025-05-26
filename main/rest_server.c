/* HTTP Restful API Server

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
 *
 * The following are registered with the server:
 * "/api/v1/system/info"       GET  retrieve system information and return in json format
 * "/api/v1/temp/raw"          GET  example code to retrieve a sample value
 * "/api/v1/system/list"       GET  retrieve a list of files and return in json format
 * "/api/v1/light/brightness"  POST used with example frontend to post a value to simulated led brightness
 * "/upload"                   GET  returns the builtin html/js for the upload drag/drop UI
 * "/upload"                   POST handles the drop event for uploading file to local embedded FS
 * "slash-star"                GET  i.e. / * no space, generic "all other" file handler; returns file contents with appropriate type
 * 
 */
#include <string.h>
#include <fcntl.h>
#include <sys/param.h>
#include <ctype.h>
#include "esp_http_server.h"
#include "esp_chip_info.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "cJSON.h"


#include "esp_littlefs.h"

#include "builtinfiles.h"


static const char *REST_TAG = "esp-rest";
#define REST_CHECK(a, str, goto_tag, ...)                                              \
    do                                                                                 \
    {                                                                                  \
        if (!(a))                                                                      \
        {                                                                              \
            ESP_LOGE(REST_TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                             \
        }                                                                              \
    } while (0)

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE (10240)
#define LOCAL_NO_CACHE

typedef struct rest_server_context {
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
} rest_server_context_t;

#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filepath)
{
    const char *type = "text/plain";
    if (CHECK_FILE_EXTENSION(filepath, ".html")) {
        type = "text/html";
    } else if (CHECK_FILE_EXTENSION(filepath, ".htm")) {
        type = "text/html";
    } else if (CHECK_FILE_EXTENSION(filepath, ".js")) {
        type = "application/javascript";
    } else if (CHECK_FILE_EXTENSION(filepath, ".css")) {
        type = "text/css";
    } else if (CHECK_FILE_EXTENSION(filepath, ".png")) {
        type = "image/png";
    } else if (CHECK_FILE_EXTENSION(filepath, ".ico")) {
        type = "image/x-icon";
    } else if (CHECK_FILE_EXTENSION(filepath, ".svg")) {
        type = "text/xml";
    }

    return httpd_resp_set_type(req, type);
}

/* Send HTTP response with the contents of the requested file */
static esp_err_t rest_common_get_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    int total_bytes_sent = 0;

    rest_server_context_t *rest_context = (rest_server_context_t *)req->user_ctx;
    strlcpy(filepath, rest_context->base_path, sizeof(filepath));
    if (req->uri[strlen(req->uri) - 1] == '/') {
        strlcat(filepath, "/index.htm", sizeof(filepath));
    } else {
        strlcat(filepath, req->uri, sizeof(filepath));
    }
    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1) {
        ESP_LOGE(REST_TAG, "Failed to open file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    set_content_type_from_file(req, filepath);

    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");

    char buffer[FILENAME_MAX];
    snprintf(buffer, sizeof(buffer), "inline; filename=\"%s\"", req->uri);
    httpd_resp_set_hdr(req, "Content-Disposition", buffer);

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
#ifdef LOCAL_NO_CACHE
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
#else
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
#endif


    char *chunk = rest_context->scratch;
    ssize_t read_bytes;
    do {
        /* Read file in chunks into the scratch buffer */
        read_bytes = read(fd, chunk, SCRATCH_BUFSIZE);
        if (read_bytes == -1) {
            ESP_LOGE(REST_TAG, "Failed to read file : %s", filepath);
        } else if (read_bytes > 0) {
            /* Send the buffer contents as HTTP response chunk */
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
                close(fd);
                ESP_LOGE(REST_TAG, "File sending failed!");
                /* Abort sending file */
                httpd_resp_sendstr_chunk(req, NULL);
                /* Respond with 500 Internal Server Error */
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                return ESP_FAIL;
            }
            else
                total_bytes_sent += read_bytes;
        }
    } while (read_bytes > 0);
    close(fd);
    ESP_LOGI(REST_TAG, "Total bytes sent = %d", total_bytes_sent);
    ESP_LOGI(REST_TAG, "File sending complete");
    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Simple handler for light brightness control */
static esp_err_t light_brightness_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = ((rest_server_context_t *)(req->user_ctx))->scratch;
    int received = 0;
    if (total_len >= SCRATCH_BUFSIZE) {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_FAIL;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    int red = cJSON_GetObjectItem(root, "red")->valueint;
    int green = cJSON_GetObjectItem(root, "green")->valueint;
    int blue = cJSON_GetObjectItem(root, "blue")->valueint;
    ESP_LOGI(REST_TAG, "Light control: red = %d, green = %d, blue = %d", red, green, blue);
    cJSON_Delete(root);
    httpd_resp_sendstr(req, "Post control value successfully");
    return ESP_OK;
}


/* file upload handler */
#define UPLOAD_POST_URI "/upload"
#define MAX_FILE_SIZE (200*1024)
#define MAX_FILE_SIZE_STR "200 KB"
#define NUM_TIMEOUTS 5
#define BODY_HEADER_END_STR "\r\n\r\n"
//#define DEBUG_DUMP_RAW 1 // if defined, dump the raw data in post request
#define FINAL_EXTRA_CHARS_AT_END 2

/*
 * look for the filename in the stream of data from the browser (buf).
 * copy the filename to the provided filename buffer (filename) and return
 * a pointer to the character of the next data byte.
 * NULL is returned if the required strings cannot be found.
 *
 * arguments:
 * char *filename - must point to a buffer into which the filename will be copied
 * char *buf - input buffer to search, remains unaltered
 * 
 * return:
 * NULL - if error
 * char *end - pointer to the first character, after the filename, in buf
 * 
 */
char *get_filename_from_body(char *filename, char *buf)  {
    char *start = NULL;
    char *end = NULL;
    char *disp = NULL;

    // Extract filename from Content-Disposition
    filename[0] = '\0';
    disp = strstr(buf, "Content-Disposition");
    if (disp) {
        start = strstr(disp, "filename=\"");
        if (start) {
            start += strlen("filename=\""); // move past 'filename="'
            end = strchr(start, '"');
            if (end) {
                strncpy(filename, start, (end-start));
                filename[(end-start)] = '\0';
                ESP_LOGI(REST_TAG, "Found filename >%s< in body", filename);
            }
            else
                return(NULL);
        }
        else
            return(NULL);
    }
    return(++end);
}


void hex_ascii_dump(const char *data, size_t len, size_t perline) {
    size_t i, j;

    for (i = 0; i < len; i += perline) {
        // Print the hex part
        printf("%08X  ", (unsigned int)i);  // Offset
        for (j = 0; j < perline; j++) {
            if (i + j < len)
                printf("%02X ", data[i + j]);
            else
                printf("   ");  // Fill in if line is short
            if (j == (perline/2 - 1)) printf(" ");  // Extra space in the middle
        }

        printf(" |");

        // Print the ASCII part
        for (j = 0; j < perline && i + j < len; j++) {
            unsigned char c = data[i + j];
            printf("%c", isprint(c) ? c : '.');
        }

        printf("|\n");
    }
}

static int parse_req_header_for_boundary(httpd_req_t *req) {
    /*
     * extract the boundary string from the req header (different from the body header),
     * and the length of the boundary header to skip when copying data.
     * NOTE: this can be done before any of the body data is read and parsed.
     * return:
     *   success : number of bytes in boundary string
     *   error: -1
     */
    char content_type[256];
    int b_str_len = -1;  // length of the boundary string
    char *boundary = NULL;
    char *b_str = NULL;  // pointer to the boundary string in content_type[]
    if(httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) == ESP_OK)  {
        ESP_LOGI(REST_TAG, "Content-Type header: %s", content_type);
        if((boundary = strstr(content_type, "boundary=")))  {
            b_str = strchr(boundary, '-');  // assume at least one dash ... find the start
            /*
             * length of actual boundary string
             * plus 2 leading dashes
             * plus 2 trailing dashes
             * plus 2 terminating \r\n
             */
            b_str_len = strlen(b_str) + 6 + FINAL_EXTRA_CHARS_AT_END;
            ESP_LOGI(REST_TAG, "boundary string length = %d", b_str_len);
        }
        else
            ESP_LOGE(REST_TAG, "\"boundary=\" not found");
    }
    else
        ESP_LOGE(REST_TAG, "Content-Type not found");
    
    return(b_str_len);
}

static esp_err_t file_upload_post_handler(httpd_req_t *req)  {

    char filepath[FILE_PATH_MAX] = {0};
    char filename[FILE_PATH_MAX] = {0};
    FILE *fd = NULL;
    char *next = NULL;
    int remaining = 0;
    struct stat file_stat;
    uint8_t timeouts = NUM_TIMEOUTS;
    bool first_read = true;  // used to do some body header parsing on the first buffer
    int b_str_len = 0;  // number of bytes in boundary string
    int b_str_bytes_to_skip = 0;

    /* Retrieve the pointer to scratch buffer for temporary storage */
    char *buf = ((rest_server_context_t *)req->user_ctx)->scratch;
    int received = 0;  // number of bytes received per tronch

    remaining = req->content_len;  // number of bytes in total from the browser
    ESP_LOGI(REST_TAG, "Total size of content = %d", remaining);

    /*
     * find the length of the multiform boundary string
     * so that it can be subtracted from the number of characters
     * to be read 
     */
    b_str_len = parse_req_header_for_boundary(req);

    esp_err_t err = ESP_OK; // exit status of the while
    while((remaining > 0) && (err == ESP_OK))  {
        ESP_LOGI(REST_TAG, "Remaining bytes before read = %d", remaining);

        /*
         * read buffer full of data 
         * leave room for the safety '\0'
         */
        timeouts = NUM_TIMEOUTS;
        do  {
            received = httpd_req_recv(req, buf, MIN(remaining, (SCRATCH_BUFSIZE-1)));
            ESP_LOGI(REST_TAG, "Number of bytes received in chunk = %d in countdown %d", received, timeouts);
            if(received == HTTPD_SOCK_ERR_TIMEOUT)
                timeouts--;
            else
                timeouts = received;  // exit with error status (always negative)
        }  while((received <= 0) && (timeouts > 0));
#ifdef DEBUG_DUMP_RAW
        ESP_LOGI(REST_TAG, "Raw contents of received buffer:");
        hex_ascii_dump(buf, received, 32);
#endif

        /*
         * if a successful read, parse out the filename
         * NOTE: assume that the first successful read it big enough to contain
         * the filename and body header information.
         */
        if(timeouts > 0)  {
            buf[received] = '\0';  // safety to make string functions work ... should be one beyond real data
            remaining -= received;  // subtract the amount that was read ... read the balance below (if any)

            /*
             * detect if this is the final buffer chunk
             * NOTE: the boundary string could be split across two reads
             * ... detect that as well
             */
            if(remaining > 0)  {
                if(remaining >= b_str_len)  // full boundary string in some subsequent read
                    b_str_bytes_to_skip = 0;
                else  // split across reads
                    b_str_bytes_to_skip = b_str_len - remaining;
            }
            else
                b_str_bytes_to_skip = b_str_len - b_str_bytes_to_skip;  //subtract number skipped last time

            if(first_read == true)  {
                first_read = false;

                /*
                * parse out the filename from the first buffer full
                */
                next = get_filename_from_body(filename, buf);  // leaves next just after the filename
                if((next != NULL) && (strlen(filename) > 0))  {
                    strncpy(filepath, ((rest_server_context_t *)req->user_ctx)->base_path, FILE_PATH_MAX);
                    strncat(filepath, filename, FILE_PATH_MAX-strlen(filepath));
                    ESP_LOGI(REST_TAG, "Upload: parsed filepath = >%s<", filepath);
                }
                else  {
                    err = ESP_FAIL;
                    break;
                }

                /*
                * adjust char *next to point to the start of actual file contents
                *
                * adjust int received to be equal to the number of bytes left to process
                * 
                * int b_str_len equals the size of the boundary string i.e. the number of
                * bytes to skip at the end.b_str_len.b_str_len.
                */
                next = strstr(next, BODY_HEADER_END_STR); //search for the best marker between filename and end of body header
                next += strlen(BODY_HEADER_END_STR); // next is now pointing to the start of actual file data
                received -= (next-buf);  // used up some number of bytes looking for filename and start of data
                ESP_LOGI(REST_TAG, "Subtracted %d bytes for filename extraction", next-buf);



                /*
                 * make sure we have a validly constructed filepath
                 */
                if (filepath[0] == '\0') {
                    /* Respond with 500 Internal Server Error */
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "filename not found");
                    err = ESP_FAIL;
                    break;
                }

                /* Filename cannot have a trailing '/' */
                if (filepath[strlen(filepath) - 1] == '/') {
                    ESP_LOGE(REST_TAG, "Invalid filename : %s", filepath);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filename");
                    err = ESP_FAIL;
                    break;
                }

                /*
                 * File cannot be larger than a limit (arbitrary)
                 */
                if (req->content_len > MAX_FILE_SIZE) {
                    ESP_LOGE(REST_TAG, "File(s) too large : %d bytes", req->content_len);
                    /* Respond with 400 Bad Request */
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                        "File size must be less than "
                                        MAX_FILE_SIZE_STR "!");
                    /*
                     * Return failure to close underlying connection else the
                     * incoming file content will keep the socket busy
                     */
                    err = ESP_FAIL;
                    break;
                }

                /*
                 * if file exists in the flash FS, delete it and continue
                 */
                if (stat(filepath, &file_stat) == 0) {
                    ESP_LOGI(REST_TAG, "File already exists ... deleting : %s", filepath);
                    unlink(filepath);
                }

                if ((fd = fopen(filepath, "w")) == NULL) {
                    ESP_LOGE(REST_TAG, "Failed to create file : %s", filepath);
                    /* Respond with 500 Internal Server Error */
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
                    err = ESP_FAIL;
                    break;
                }

                ESP_LOGI(REST_TAG, "Ready to receiving file : %s...", filename);
            } // if first read
            else
                next = buf;  // no header characters to skip after first

            /*
             * subtract the full or partial boundary string length
             */
            received -= b_str_bytes_to_skip;  // don't read past the actual data into the boundary string
            ESP_LOGI(REST_TAG, "Applying b_str_bytes value of = %d", b_str_bytes_to_skip);

            /*
             * if we've gotten this far:b_str_len
             * - req header parsed for total number of bytes and boundary string set
             * - first bunch of data read
             * - file name/path validated
             * - file opened and ready to receive data
             * now: copy the data to the newly opened file
             */
            if(fwrite(next, 1, received, fd) != received) {
                ESP_LOGE(REST_TAG, "Error writing chunk of data to file");
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
                fclose(fd);
                unlink(filepath);
                return ESP_FAIL;
            }
        }  // if successful read i.e. timeouts > 0
        else
            err = ESP_FAIL;

    }  // while() ... reading loop

    /* Close file upon upload completion */
    fclose(fd);
    ESP_LOGI(REST_TAG, "File reception complete");

    return(err);
}

/* Simple handler for getting system handler */
static esp_err_t system_info_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    cJSON_AddStringToObject(root, "version", IDF_VER);
    cJSON_AddNumberToObject(root, "cores", chip_info.cores);
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

/* Simple handler for getting temperature data */
static esp_err_t temperature_data_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "raw", esp_random() % 20);
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t upload_handler(httpd_req_t *req)  {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, uploadContent);
    return ESP_OK;
}


// This function is called when the WebServer was requested to list all existing files in the filesystem.
// a JSON array with file information is returned.
#include "dirent.h"
#include "sys/stat.h"
#define LIST_PATH "/littlefs"
#define FN_BUFSIZE 256
esp_err_t list_files_handler(httpd_req_t *req) {
    DIR *dir;
    struct dirent *entry;
    struct stat stat_info;
    char full_path[FN_BUFSIZE] = {0};

    httpd_resp_set_type(req, "application/json");

    if((dir = opendir(LIST_PATH)) == NULL)  {
        ESP_LOGE(REST_TAG, "Error opening %s for listing", LIST_PATH);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filename");
        return(ESP_FAIL);
    }

    cJSON *json_array = cJSON_CreateArray();
    while ((entry = readdir(dir)) != NULL) {
        strncpy(full_path, LIST_PATH, FN_BUFSIZE);
        strncat(full_path, "/", (FN_BUFSIZE-strlen(full_path)));
        strncat(full_path, entry->d_name, (FN_BUFSIZE-strlen(full_path)));
        stat(full_path, &stat_info);
        cJSON *file_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(file_obj, "name", entry->d_name);
        cJSON_AddNumberToObject(file_obj, "size", stat_info.st_size);

        // Add to array
        cJSON_AddItemToArray(json_array, file_obj);
    }
    char *json_str = cJSON_Print(json_array);
    httpd_resp_sendstr(req, json_str);
    if(json_str != NULL) free(json_str);
    closedir(dir);
    cJSON_Delete(json_array);

    return(ESP_OK);

}  // list_files_handler()



esp_err_t start_rest_server(const char *base_path)
{
    REST_CHECK(base_path, "wrong base path", err);
    rest_server_context_t *rest_context = calloc(1, sizeof(rest_server_context_t));
    REST_CHECK(rest_context, "No memory for rest context", err);
    strlcpy(rest_context->base_path, base_path, sizeof(rest_context->base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(REST_TAG, "Starting HTTP Server");
    REST_CHECK(httpd_start(&server, &config) == ESP_OK, "Start server failed", err_start);

    /* URI handler for fetching system info */
    httpd_uri_t system_info_get_uri = {
        .uri = "/api/v1/system/info",
        .method = HTTP_GET,
        .handler = system_info_get_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_info_get_uri);

    /* URI handler for fetching temperature data */
    httpd_uri_t temperature_data_get_uri = {
        .uri = "/api/v1/temp/raw",
        .method = HTTP_GET,
        .handler = temperature_data_get_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &temperature_data_get_uri);

    
    /* URI handler for light brightness control */
    httpd_uri_t list_file_get_uri = {
        .uri = "/api/v1/system/list",
        .method = HTTP_GET,
        .handler = list_files_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &list_file_get_uri);

    /* URI handler for light brightness control */
    httpd_uri_t light_brightness_post_uri = {
        .uri = "/api/v1/light/brightness",
        .method = HTTP_POST,
        .handler = light_brightness_post_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &light_brightness_post_uri);


    /* URI handler for file uploads */
    httpd_uri_t upload_uri = {
        .uri = UPLOAD_POST_URI,
        .method = HTTP_GET,
        .handler = upload_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &upload_uri);

    /* URI handler for file upload return post */
    httpd_uri_t file_upload_post_uri = {
        .uri = UPLOAD_POST_URI,
        .method = HTTP_POST,
        .handler = file_upload_post_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &file_upload_post_uri);

    /* URI handler for getting web server files */
    httpd_uri_t common_get_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = rest_common_get_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &common_get_uri);


    return ESP_OK;
err_start:
    free(rest_context);
err:
    return ESP_FAIL;
}
