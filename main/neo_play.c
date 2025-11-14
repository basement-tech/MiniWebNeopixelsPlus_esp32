/*
 * neo_play.c
 *
 * functions to play out the neo_pixel patterns using the state machine
 * implemented in void neo_cycle_next(void).  this function is expected to be called/controlled
 * by an process external to this file.
 * 
 * the various sequence strategies are handled via the pointers to
 * functions provided in the switch table seq_callbacks_t seq_callbacks[NEO_SEQ_STRATEGIES].
 * 
 * Other functions are provided to handle the interface with the web server (via IPC) and
 * utility functions to open, read and parse the contents of file driven sequences.
 * 
 * hooks to a script engine can be included by definining INCLUDE_SCRIPT_HOOKS.  otherwise
 * this function should stand alone for processing requests from web server to play out
 * sequences.
 * 
 */

#include <string.h>
#include <stdint.h>
#include <sys/param.h>
#include <ctype.h>
#include <fcntl.h>
#include "esp_random.h"  // encryption grade random number generator
#include "esp_vfs.h"  //resolves read, close
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"  // high res timer
#include "driver/gptimer.h"  // general purpose timer
#include "json_parser.h"

#include "neo_system.h"
#include "neo_ll_api.h"
#include "neo_data.h"
#include "neo_parsing.h"
#include "servo_defs.h"
#include "neo_script.h"

#include "neo_script.h"

#define TAG "neo_play"

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)

/*
 * housekeeping for the sequence state machine
 */
seq_callbacks_t seq_callbacks[];
#define NEO_SEQ_START    0
#define NEO_SEQ_WAIT     1
#define NEO_SEQ_WRITE    2
#define NEO_SEQ_STOPPING 3
#define NEO_SEQ_STOPPED  4
static uint8_t neo_state = NEO_SEQ_STOPPED;  // state of the cycling state machine

seq_strategy_t current_strategy = SEQ_STRAT_POINTS;

uint64_t current_millis = 0; // mS of last update
int32_t current_index = 0;   // index into the pattern array
neo_script_cmd_t pending_script_cmd = NEO_CMD_SCRIPT_UNDEFINED; // async button input waiting for next state machine cycle

/*
 * set up and request a new sequence via the mutex protected global structure
 */
int8_t neo_request_sequence(char *label, char *filename)  {
  int8_t ret = NEO_SUCCESS;
  strncpy(neo_mutex_data.sequence, label, MAX_NEO_SEQUENCE);
  strncpy(neo_mutex_data.file, filename, MAX_FILENAME);
  neo_mutex_data.resp_reqd = false;  // this sequence not coming from web client
  neo_mutex_data.new_data = true;
  if(xSemaphoreGive(xneoMutex) != pdTRUE)
    ret = NEO_MUTEX_ERR;
  return(ret);
}

/*
 * compatibility/porting convenience
 */
uint64_t millis(void)  {
  return(esp_timer_get_time()/1000);
}

/*
 * define the general purpose timer for running the state machine
 */
static bool neo_timer_on_alarm_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)  {
  static BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(xneo_cycle_next_flag, &xHigherPriorityTaskWoken);
  return(true);
}
#define INTR_SQWAVE_FREQ   1000000  // timer frequency 1MHz, 1 tick=1us
void neo_timer_setup(void)  {
    ESP_LOGI(TAG, "Create state machine timer handle");
    gptimer_handle_t gptimer = NULL;
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = INTR_SQWAVE_FREQ,
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));

    gptimer_event_callbacks_t cbs = {
        .on_alarm = neo_timer_on_alarm_cb,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, NULL));

    ESP_LOGI(TAG, "Enable state machine timer with frequency of %d Hz", INTR_SQWAVE_FREQ);
    ESP_ERROR_CHECK(gptimer_enable(gptimer));

    ESP_LOGI(TAG, "Start state machine timer, period is %d uS", NEO_UPDATE_INTERVAL);
    gptimer_alarm_config_t alarm_config1 = {
        .reload_count = 0,
        .alarm_count = NEO_UPDATE_INTERVAL, // period
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config1));
    ESP_ERROR_CHECK(gptimer_start(gptimer));
}


/*
 * return the index in neo_sequences[] that matches
 * the label given as an argument.  Do *not* set the global
 * index value that is used to play the sequence.
 */
int8_t neo_find_sequence(const char *label)  {
  int8_t ret = -1;
  for(int i = 0; i < MAX_SEQUENCES; i++)  {
    if(strcmp(label, neo_sequences[i].label) == 0)
      ret = i;
  }
  return(ret);
}

/*
 * return the index in neo_file_procs[] that matches
 * the filetype given as an argument.
 */
int8_t neo_find_filetype(const char *filetype)  {
  int8_t ret = -1;
  for(int i = 0; (neo_file_procs[i].filetypes[0] != '\0'); i++)  {
    if(strcmp(filetype, neo_file_procs[i].filetypes) == 0)
      ret = i;
  }
  return(ret);
}


/*
 * which/set sequence are we playing out
 * returns: -1 if the label doesn't match a sequence
 * reset the playout index and state if the found index
 * is different than the currently running index.
 */
int8_t seq_index = -1;  // global used to hold the index of the currently running sequence
int8_t neo_set_sequence(const char *label, const char *strategy)  {
  int8_t ret = NEO_SUCCESS;
  int8_t new_index = 0;
  seq_strategy_t new_strat;


  /*
   * attempt to set the sequence
   */
  new_index = neo_find_sequence(label);
  ESP_LOGI(TAG, "neo_find_sequence returned new_index = %d", new_index);
  if(new_index >= 0)  {
    /*
     * is this a new sequence from the one running?
     */
    if(new_index != seq_index)  {  // if true, yes
      // ADD STOP HERE
      ret = NEO_NEW_SUCCESS;
      seq_index = new_index;  // set the sequence index that is to be played

      /*
       * if sequence setting was successful, attempt to set the strategy
       *
       * allow for the strategy argument to be a null string so,
       * for example, in the case of a built-in it might remain
       * the initialized value
       */
      if(strategy[0] == '\0')  {  // built-in
        if((new_strat = neo_set_strategy(neo_sequences[seq_index].strategy)) == SEQ_STRAT_UNDEFINED)
          ret = NEO_STRAT_ERR;
        else
          ESP_LOGI(TAG, "Using built-in strategy %d", new_strat);
      }
      else {  // user sequence
        if((new_strat = neo_set_strategy(strategy)) == SEQ_STRAT_UNDEFINED)
          ret = NEO_STRAT_ERR;
        else
          ESP_LOGI(TAG, "Using USER strategy %d", new_strat);
      }

      /*
       * if all above was successful, set up the globals and start the sequence
       */
      if(ret == NEO_NEW_SUCCESS)  {
        current_index = 0;  // reset the pixel count
        current_strategy = new_strat;
        ESP_LOGI(TAG, "neo_set_sequence: set sequence to %d and strategy to %d\n", seq_index, current_strategy);
        neo_state = NEO_SEQ_START;  // cause the state machine to start at the start
      }
    }
    else
      ret = NEO_OLD_SUCCESS; 
  }
  else
    ESP_LOGE(TAG, "neo_set_sequence: Invalid sequence label");
  return(ret);
}

/*
 * check if the label matches a predefined USER button
 * NOTE: this was simplified when the filename attribute was
 * added to the html file
 */
int8_t neo_is_user(const char *label)  {
  int8_t ret = NEO_FILE_LOAD_NOTUSER;

  if(strncmp(label, "USER", 4) == 0)
    ret = NEO_SUCCESS;

  return(ret);
}

/*
 * display the printable contents of char buf[],
 * and a count of the number of other binary data
 * (good for bitwise binary for example)
 */
void disp_printable(char *buf, int16_t size)  {
  int16_t nonprint = 0;

  while(size-- > 0)  {
    if(isprint((int)(*buf)) || (*buf == '\n')  || (*buf == '\r'))
      putc(*buf, stdout);
    else
      nonprint++;
    buf++;
  }
  printf("\n... plus %d unprintable\n\n", nonprint);
}

/*
 * look for a label matching the argument, label,
 * and load a sequence from file of the same name.
 * NOTE: currently the requested sequence placeholder of the name
 * requested must exist in neo_sequences[] for this to succeed.
 * 
 * finally set the newly loaded sequence as current and start it.
 * 
 * details:
 *   - stat the filesystem to make sure it's readable
 *   - make sure the file exists and open
 *   - read the file contents, close
 *   - mark the end of the first line
 *   - json parse the first line to determine file type
 *   - using the switch table neo_file_procs[filetype_idx], call the .data_valid() to validate data
 *   - using the switch table neo_file_procs[filetype_idx], call the .neo_proc_seqfile()
 *     to parse the balance of the file  (buffer pointer passed is pointing after preamble line)
 *   - launch the newly loaded sequence
 *
 * return:   0: successfully loaded
 *          -1: file not found or error opening
 *          -2: error deserializing file
 */
char buf[NEO_MAX_SEQ_FILE_SIZE] = {0};  // buffer in which to read the file contents
int8_t neo_load_sequence(const char *file)  {

  int8_t ret = NEO_SUCCESS;

  struct stat file_stat;

  char *pbuf_data = NULL;  // pointer in buf[] after the preamble
  char filepath[FILE_PATH_MAX] = {0};  // fully qualified path to file

  jparse_ctx_t jctx;  // for json parsing

  char filetype[16];  // file type as string extracted from json first row
  int json_len = 0;  // size of the balance of the json part of the file
  uint16_t hdr_len;  // length of the preamble json string
  uint16_t bin_len;  // calculated size of binary data

  script_mutex_data_t script_info;  // in case the file is a script


  /*
   * verify access to the filesystem by displaying partition info
   */
  size_t total = 0, used = 0;
  if(esp_littlefs_info(LITTLE_FS_PARTITION_LABEL, &total, &used) != ESP_OK)  {
    ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
    return(NEO_FILE_LOAD_OTHER);
  } 
  else {
    ret = NEO_SUCCESS;
    ESP_LOGI(TAG, "Filesystem Partition size: total: %d, used: %d", total, used);
  }

  /*
   * create the fully qualified path to the file
   */
  strncpy(filepath, LITTLE_FS_MOUNT_POINT, FILE_PATH_MAX);
  strncat(filepath, "/", (FILE_PATH_MAX - strlen(filepath)));
  strncat(filepath, file, (FILE_PATH_MAX - strlen(filepath)));

  /*
   * verify that the file exists and if so,
   * read the contents of the user sequence file and put it
   * in the character buffer buf.  deserialize the json and 
   * load it into the sequence array.
   */

  if (stat(file, &file_stat) == 0)  {
      ESP_LOGE(TAG, "ERROR: Filename %s does not exist in file system\n", file);
      ret = NEO_FILE_LOAD_NOFILE;
  }
  else  {

    ESP_LOGI(TAG, "Loading filename %s ...\n", file);

    /*
     * switching to fopen(), fread() in hopes that the 
     * buffering will be more thread-safe.
     * Not sure it helped, but I'll leave it since it functions
     * properly.
     */
    FILE *fp = fopen(filepath, "rb");  // -> OPEN FILE
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open file : %s", filepath);
        return NEO_FILE_LOAD_OTHER;
    }
    else  {
      /*
       * read the file contents into buf and close the file
       */
      int read_bytes = fread(buf, 1, sizeof(buf), fp);
      buf[read_bytes] = '\0';  // terminate the char string
      fclose(fp);                      // -> CLOSE FILE
      //ESP_LOGI(TAG, "Raw file contents:\n%s\n", buf);  // char *buf takes it from here
      /*
       * show the printable parts of the file
       */
      ESP_LOGI(TAG, "Raw file contents:");
      disp_printable(buf, read_bytes);

      /*
       * mark the start of the data after the preamble line with char *pbuf_data
       *
       * seems that the files have \r\n (od -c) at the end of each line,
       * but when reading it seems that it's only one char.
       */
      pbuf_data = NULL;
      for(hdr_len = 0; hdr_len < read_bytes; hdr_len++)  {
          if(buf[hdr_len] == '\n')  {
            pbuf_data = buf + ++hdr_len;
            break;
          }
      }

      /*
       * if we got to the end of the buffer with no newline,
       * the file is corrupt ... no guess possible.
       */
      if(hdr_len >= read_bytes)  {
        ESP_LOGE(TAG, "Error: No preamble line present");
        ret = NEO_FILE_LOAD_OTHER;
      }
      else  {
        /*
         * parse the header first to determine file type
         */
        if(json_parse_start(&jctx, buf, hdr_len) != OS_SUCCESS)  {
          ESP_LOGE(TAG, "ERROR: Deserialization of preamble failed ... don't know file type");
          ret = NEO_DESERR;
        }
        else  {
          if(json_obj_get_string(&jctx, "filetype", filetype, sizeof(filetype)) != OS_SUCCESS)  {
            ESP_LOGE(TAG, "ERROR: Header does not contain \"filetype\" ... don't know file type");
            ret = NEO_DESERR;
          }
          else if(json_obj_get_int(&jctx, "jsonlen", &json_len) != OS_SUCCESS)  {
            ESP_LOGE(TAG, "ERROR: Header does not contain \"jsonlen\"");
            ret = NEO_DESERR;
          }
          else  {
            ESP_LOGI(TAG, "Preamble filetype determined = \"%s\"", filetype);

            int8_t filetype_idx = -1;
            if((filetype_idx = neo_find_filetype(filetype)) < 0)
            {
              ESP_LOGE(TAG, "ERROR: neo_load_sequence: no placeholder for %s in filetype array\n", filetype);
              ret = NEO_FILE_LOAD_NOPLACE;
            }
            else
            {
              /*
               * most of this is ignored by "OG", json only ascii data file
               */
              ESP_LOGI(TAG, "parsing balance of sequence file base on filetype %s", filetype);
              ESP_LOGI(TAG, "total bytes in file = %d", read_bytes);
              ESP_LOGI(TAG, "minus header length   %d", hdr_len);
              ESP_LOGI(TAG, "minus json header     %d", json_len);
              ESP_LOGI(TAG, "                    ------");
              bin_len = read_bytes-hdr_len-json_len;
              ESP_LOGI(TAG, "binary/bitwise data   %d\n", bin_len);
              if(neo_file_procs[filetype_idx].data_valid(&bin_len) != true)  {
                ESP_LOGE(TAG, "ERROR: size of binary data indicates file is malformed");
                ret = NEO_FILE_LOAD_OTHER;
              }
              else
                ret = neo_file_procs[filetype_idx].neo_proc_seqfile(pbuf_data, json_len, bin_len);
            }
          }
          json_parse_end(&jctx);
        }
      }
    }
  }
  return(ret);
}

/*
 * helper for writing a single color to all pixels
 */
void neo_write_pixel(bool clear)  {
  if(clear != 0)  pixels_clear(); // Set all pixel colors to 'off'

  /*
    * send the next point in the sequence to the strand
    */
  for(int i=0; i < pixels_numPixels(); i++) { // For each pixel...
    pixels_setPixelColorRGB(i, neo_sequences[seq_index].point[current_index].red, 
                               neo_sequences[seq_index].point[current_index].green,
                               neo_sequences[seq_index].point[current_index].blue, 0);
  }
  pixels_show();   // Send the updated pixel colors to the hardware.
}

/*
 * blink a status color to the strip reps times
 * the color is from the Adafruit colorwheel representation of colors
 * t is the numbe of mS between changes on/off (i.e. blink rate/2)
 *
 * NOTE: this is a blocking function i.e. not suitable for use in the loop()
 * NOTE: the neopixel strand must have been initialized prior to calling this function
 * NOTE: this is not a thread safe function
 *
 */
void neo_n_blinks(uint8_t r, uint8_t g, uint8_t b, uint8_t w, int8_t reps, int32_t t)  {

  for(int8_t j = reps; j > 0; j--)  {
    /*
    * send the next point in the sequence to the strand
    */
    for(int i=0; i < pixels_numPixels(); i++) // For each pixel...
      pixels_setPixelColorRGB(i, r, g, b, w);
    pixels_show();   // Send the updated pixel colors to the hardware.

    vTaskDelay(t / portTICK_PERIOD_MS);

    pixels_clear();
    pixels_show();
    
    vTaskDelay(t / portTICK_PERIOD_MS);
  }
}

/*
 * initialize strand to a known state and set it to state machine to off/idle
 */
void neo_init(void)  {

  pixels_clear(); // Set all pixel colors to 'off'
  pixels_show();   // Send the updated pixel colors to the hardware.
  neo_state = NEO_SEQ_STOPPED;
  xSemaphoreTake(xneo_cycle_next_flag, portMAX_DELAY);  // should be immediately available at startup


  neo_timer_setup();  // start the state machine timer
}

/*
 * TODO: figure out a better way to do this
 * __asm__ __volatile__("" ::: "memory") is an assembly
 * language directive to keep the empty function from being
 * optimized in any way:
 * __asm__ or asm: GCC keyword for inline assembly
 * __volatile__: Prevents the compiler from optimizing the statement away
 * "": Empty assembly instruction — this means no actual machine instruction is generated
 * ::: "memory": Declares a "memory clobber", which tells the compiler
 */
//void noop(void) {vTaskDelay(10/portTICK_PERIOD_MS);}
void noop(void) {__asm__ __volatile__("" ::: "memory");}
void start_noop(bool clear) {}


/*
 * SEQ_STRAT_POINTS
 * each line in the json is a single point in the sequence.
 * the "t" times are mS between points
 * the sequence restarts at the end and runs continuously
 *
 * bonus: none
 *
 * NOTE: keep neo_points_start() in sync with neo_single_start()
 * when making changes.
 */
void neo_points_start(bool clear) {
  neo_write_pixel(true);  // clear the strand and write the first value
  current_millis = millis();
  neo_state = NEO_SEQ_WAIT;
}

void neo_points_write(void) {
  if(neo_sequences[seq_index].point[current_index].ms_after_last < 0)  // list terminator: nothing to write
    current_index = 0;
  neo_write_pixel(false);
  neo_state = NEO_SEQ_WAIT;
}

void neo_points_wait(void)  {
  uint64_t new_millis = 0;

  /*
    * if the timer has expired (or assumed that if current_millis == 0, then it will be)
    * i.e. done waiting move to the next state
    */
  if(((new_millis = millis()) - current_millis) >= neo_sequences[seq_index].point[current_index].ms_after_last)  {
    current_millis = new_millis;
    current_index++;
    neo_state = NEO_SEQ_WRITE;
  }

}

void neo_points_stopping(void)  {
   /*
    * move to top to avoid potential collision with late
    * coming meo_cycle_next()
    */
  neo_state = NEO_SEQ_STOPPED;

  pixels_clear(); // Set all pixel colors to 'off'
  pixels_show();   // Send the updated pixel colors to the hardware.
  current_index = 0;  // housekeepinb
  seq_index = -1; // so it doesn't match
  current_strategy = SEQ_STRAT_POINTS;  // housekeeping
}

// end of SEQ_STRAT_POINTS callbacks

/*
 * SEQ_STRAT_SINGLE
 * each line in the json is a single point in the sequence.
 * the "t" times are mS between points
 * the sequence runs the number of times given by the "count"
 * json parameter in the bonus string.
 *
 * NOTE: keep neo_points_start() in sync with neo_single_start()
 * when making changes.
 */
static int8_t single_repeats = 1;

void neo_single_start(bool clear) {
  jparse_ctx_t jctx;  // for json parsing
  char jbuf[MAX_NEO_BONUS];

  neo_write_pixel(true);  // clear the strand and write the first value

  /*
   * if the number of repeats given in the bonus value
   * seems to be valid use it to set the number of 
   * times the sequence is repeated, otherwise just indicate
   * that the sequence should be played once.
   */

  /*
   * obtain the number of times the "single" sequence will be run
   * based on the "bonus" parameter from the json sequence file
   */
  if(strlen(neo_sequences[seq_index].bonus) > 0)  {
    ESP_LOGD(TAG, "neo_single_start: bonus = %s", neo_sequences[seq_index].bonus);

    if(json_parse_start(&jctx, neo_sequences[seq_index].bonus, strlen(neo_sequences[seq_index].bonus)) != OS_SUCCESS)  {
      ESP_LOGE(TAG, "ERROR: Deserialization of bonus failed ... using zero\n");
      single_repeats = 1;  // default to 1 time through
    }
    else  {
      json_obj_get_string(&jctx, "count", jbuf, sizeof(jbuf));  // used to point to place in sequence array

      if(*jbuf == '\0')  {
        ESP_LOGE(TAG, "WARNING: slowp bonus has no member \"count\" ... using one\n");
        single_repeats = 1;
      }
      else  {
        if(atoi(jbuf) > INT8_MAX) 
          single_repeats = INT8_MAX;
        else
          single_repeats = atoi(jbuf);
        ESP_LOGI(TAG, "neo_single_start: single_repeats set to %d", single_repeats);
      }
      json_parse_end(&jctx);  // done with json
    }
  }
  else
    single_repeats = 1;

  /*
   * get the timing started
   */
  current_millis = millis();
  neo_state = NEO_SEQ_WAIT;
}

void neo_single_write(void) {
  if(neo_sequences[seq_index].point[current_index].ms_after_last < 0)  {  // list terminator
    current_index = 0;  // rewind in case we're going to play it again
    if(--single_repeats > 0)  {  // are we going to play it again?
      neo_state = NEO_SEQ_WAIT;  // yep
      neo_write_pixel(false);
    }
    else
      neo_state = NEO_SEQ_STOPPING;  // nope
  }
  else  {  // just write the point and continue
    neo_write_pixel(false);
    neo_state = NEO_SEQ_WAIT;
  }
}

// end of SEQ_STRAT_SINGLE callbacks

/*
 * SEQ_STRAT_SLOWP
 * this is a slowly moving pulse sequence
 * only a two points are expected in the json, from which
 * the endpoint/maximum (color and intensity)  and the starting intensity
 * of the pulse is taken
 *
 * "t" from the first line is interpreted a the total number of seconds for the wave
 * this is a calculated sequence (based on NEO_SLOWP_POINTS):
 * - the interval between changes is based on the "t" seconds parameter
 * - the delta change is calculated
 *
 * "bonus"  from the json sequence file is itself a little json string
 * that indicates as "count" the number of flickers and ["flicker"]["r"], ["g"] and ["b"],
 * the color of the flicker (could be 0, 0, 0 for dark or 255, 255, 255 for bright, for example).
 * the ["flicker"]["t"] might be used someday for the duration of the flicker,
 * especially for very long running fades.
 */
static int32_t slowp_idx = 0;  // counting through the NEO_SLOWP_POINTS
static int8_t slowp_dir = 1;  // +1 -1 to indicate the direction we're traveling
static uint32_t delta_time;  // calculated time between changes
static float delta_r, delta_g, delta_b;  // calculated increment for each color ... must be floats or gets rounded to 0 between calls
static float slowp_r, slowp_g, slowp_b;  // remember where we are in the sequence
static int16_t slowp_flickers[NEO_SLOWP_FLICKERS];  // random points to flicker
static int16_t slowp_flicker_idx = 0;
static int8_t flicker_count = 0;  // how many flickers
static uint8_t flicker_r, flicker_g, flicker_b;  // colors to flicker to

/*
 * Comparison function for qsort (seems that there's an "int" somewhere
 * buried that causes an invalid conversion of int16_t is used for type)
 * TODO: figure it out whilst wasting 200 bytes of RAM ... solved:
 * function must return an int no matter what types are being compared ... implemented, works.
 */
static int compare_int16_t(const void *a, const void *b) {
    int16_t c = *(int16_t*)a;
    int16_t d = *(int16_t*)b;
    return ((int)(c - d));
}

static uint8_t neo_check_range(int32_t testval)  {
  uint8_t retval = testval;
  if(testval < 0) retval = 0;
  if(testval > 255)  retval = 255;
  return(retval);
}

void neo_slowp_start(bool clear)  {

  slowp_idx = 0;
  slowp_dir = 1;  // start by going up
  slowp_flicker_idx = 0;  // start at the start
  flicker_count = 0;  // assume none to Start

  jparse_ctx_t jctx;  // for json parsing
  char jbuf[MAX_NEO_BONUS];
  int err = OS_SUCCESS;

  /*
   * calculate delta time in mS based on the first (and only)
   * line in the json sequence file
   */
  delta_time = (neo_sequences[seq_index].point[0].ms_after_last * 1000) / NEO_SLOWP_POINTS;

  /*
   * calculate the delta chance for each color
   * the first line in the json sequence has the max/endpoint
   * of the sequence
   *
   */
  delta_r = (neo_sequences[seq_index].point[1].red - neo_sequences[seq_index].point[0].red) / (float)NEO_SLOWP_POINTS;  // cast needed to force floating point math
  delta_g = (neo_sequences[seq_index].point[1].green - neo_sequences[seq_index].point[0].green) / (float)NEO_SLOWP_POINTS;
  delta_b = (neo_sequences[seq_index].point[1].blue - neo_sequences[seq_index].point[0].blue) / (float)NEO_SLOWP_POINTS;

  /*
   * start from the json specified starting point
   */
  slowp_r = neo_sequences[seq_index].point[0].red;
  slowp_g = neo_sequences[seq_index].point[0].green;
  slowp_b = neo_sequences[seq_index].point[0].blue;

  /*
   * obtain the random places where the lights will flicker
   * based on the "bonus" parameter from the json sequence file
   */
  if(strlen(neo_sequences[seq_index].bonus) > 0)  {
    ESP_LOGD(TAG, "neo_slowp_start: bonus = %s", neo_sequences[seq_index].bonus);

    if(json_parse_start(&jctx, neo_sequences[seq_index].bonus, strlen(neo_sequences[seq_index].bonus)) != OS_SUCCESS)  {
      ESP_LOGE(TAG, "ERROR: Deserialization of bonus failed ... using zero\n");
      flicker_count = 0;  // default to 1 time through
    }
    else  {
      json_obj_get_string(&jctx, "count", jbuf, sizeof(jbuf));  // used to point to place in sequence array

      if(*jbuf == '\0')  {
        ESP_LOGE(TAG, "WARNING: slowp bonus has no member \"count\" ... using one\n");
        flicker_count = 0;
      }
      else  {
        if(atoi(jbuf) > INT8_MAX)
          flicker_count = INT8_MAX;
        else
          flicker_count = atoi(jbuf);
        ESP_LOGI(TAG, "neo_slowp_start: flicker_count set to %d", flicker_count);

        /*
         * what color should the flickers be
         * in case colors are missing, set the ones we have
         */
        flicker_r = flicker_g = flicker_b = 255;  // set fallback
        if((err = json_obj_get_object(&jctx, "flicker")) != OS_SUCCESS)
          ESP_LOGE(TAG, "WARNING: slowp bonus has incomplete member \"flicker\" ... using white\n");
        else  {
          if(json_obj_get_string(&jctx, "r", jbuf, sizeof(jbuf)) == OS_SUCCESS)
            flicker_r = neo_check_range(atoi(jbuf));

          if(json_obj_get_string(&jctx, "g", jbuf, sizeof(jbuf)) == OS_SUCCESS)
            flicker_g = neo_check_range(atoi(jbuf));

          if(json_obj_get_string(&jctx, "b", jbuf, sizeof(jbuf)) == OS_SUCCESS)
            flicker_b = neo_check_range(atoi(jbuf));

          json_obj_leave_object(&jctx);
        }
        ESP_LOGI(TAG, "Setting slowp rgb color to (%u %u %u)\n", flicker_r, flicker_g, flicker_b);
      }
      json_parse_end(&jctx);  // done with json
    }
  }
  else
    flicker_count = 0;

  /*
   * boundary check and legacy where negative was ok
   */
  if(abs(flicker_count) > NEO_SLOWP_FLICKERS) flicker_count = NEO_SLOWP_FLICKERS;  //boundary check
  flicker_count = abs(flicker_count);  // legacy

  /*
   * calculate where to put the flickers based on a random number
   *
   * esp_random() returns uin32_t; slowp_flickers[] contains step number
   * in the sequence where to substitute a flicker.  The step numbers are
   * between 0 and NEO_SLOWP_POINTS.  So, divide by the difference to pull
   * the randoms into the right range.
   */
  static uint32_t scale = UINT32_MAX/NEO_SLOWP_POINTS;
  for(uint8_t j = 0; j < flicker_count; j++)  {
    slowp_flickers[j] = esp_random()/scale;
    if(slowp_flickers[j] == (int16_t)0)
      slowp_flickers[j] = 1;  // stay away from the turn-arounds
    else if(slowp_flickers[j] == (int16_t)(NEO_SLOWP_POINTS-1))
      slowp_flickers[j] = (NEO_SLOWP_POINTS-2);
  }

  ESP_LOGD(TAG, "Starting slowp: dr = %f, dg = %f, db = %f dt = %lu\n", delta_r, delta_g, delta_b, delta_time);
  ESP_LOGD(TAG, "Randoms are (unsorted):");
  for(uint8_t j = 0; j < flicker_count; j++)
    ESP_LOGD(TAG, "%d  ", slowp_flickers[j]);
  ESP_LOGD(TAG, "\n");

  /*
   * Sort the array in place
   * TODO: strange that it seems to only work with ints despite
   * all of the syntax to the contrary? ... solved: compare function must
   * return an int no matter which type is being sorted: implemented, works.
   */
  qsort(slowp_flickers, flicker_count, sizeof(int16_t), compare_int16_t);

  ESP_LOGD(TAG, "Randoms are (sorted):");
  for(uint8_t j = 0; j < flicker_count; j++)
    ESP_LOGD(TAG, "%d  ", slowp_flickers[j]);
  ESP_LOGD(TAG, "\n");

  uint8_t r = neo_check_range(slowp_r);
  uint8_t g = neo_check_range(slowp_g);
  uint8_t b = neo_check_range(slowp_b);

  /*
   * clear and write the starting value
   */
  pixels_clear();
  for(int i=0; i < pixels_numPixels(); i++)  // For each pixel...
      pixels_setPixelColorRGB(i, r, g, b, 0);
  pixels_show();   // Send the updated pixel colors to the hardware.

  current_millis = millis();

  neo_state = NEO_SEQ_WAIT;

}


void neo_slowp_write(void) {
  uint8_t r, g, b;

  //DEBUG_DEBUG("slowp_idx = %d\n", slowp_idx); // warning: burps out a lot of stuff

  /*
   * currently going up
   */
  if(slowp_dir > 0)  {
    if(++slowp_idx < NEO_SLOWP_POINTS)  {  // have not reached the top of the sequence
      slowp_r += delta_r;  // increment by the delta per point change
      slowp_g += delta_g;
      slowp_b += delta_b;
    }
    else  {
      slowp_dir = -1;  // change to going down
      slowp_idx--;  // decrement back to the top since we incremented past

      /*
       * reset to the ending point in case of rounding error
       */
      slowp_r = neo_sequences[seq_index].point[1].red;
      slowp_g = neo_sequences[seq_index].point[1].green;
      slowp_b = neo_sequences[seq_index].point[1].blue;
    }
  }

  /*
   * currently going down
   */
  else  {
    if(--slowp_idx >= 0)  { 
      slowp_r -= delta_r;
      slowp_g -= delta_g;
      slowp_b -= delta_b;
    }
    else  {
      slowp_dir = 1;  // change to going down
      slowp_idx++;

      /*
       * reset to the starting point  in case of rounding error
       */
      slowp_r = neo_sequences[seq_index].point[0].red;
      slowp_g = neo_sequences[seq_index].point[0].green;
      slowp_b = neo_sequences[seq_index].point[0].blue;
    }
  }

  /*
   * send the next point in the sequence to the strand
   */
  if(flicker_count == 0)  {  // no flickers
          r = neo_check_range(slowp_r);
          g = neo_check_range(slowp_g);
          b = neo_check_range(slowp_b);
  }
  else  {
    if(slowp_idx == slowp_flickers[slowp_flicker_idx])  {
        r = flicker_r;
        g = flicker_g;
        b = flicker_b;

      if(slowp_dir > 0)  {
        if(++slowp_flicker_idx >= flicker_count)
          slowp_flicker_idx = flicker_count - 1;
      }
      else  {
        if(--slowp_flicker_idx < 0)
          slowp_flicker_idx = 0;
      }
//      DEBUG_DEBUG("slowp_flicker_idx = %d\n", slowp_flicker_idx);
    }
    else  {
      r = neo_check_range(slowp_r);
      g = neo_check_range(slowp_g);
      b = neo_check_range(slowp_b);

    }
  }
  for(int i=0; i < pixels_numPixels(); i++)  // For each pixel...
      pixels_setPixelColorRGB(i, r, g, b, 0);

  pixels_show();   // Send the updated pixel colors to the hardware.

#ifdef DEBUG_HACK
  ESP_LOGD(TAG, "neo_slowp_write: Showed %d  %d  %d\n", slowp_r, slowp_g, slowp_b);
  while(Serial.available() == 0);
  Serial.read();
#endif

  neo_state = NEO_SEQ_WAIT;
}


void neo_slowp_wait(void)  {
  uint64_t new_millis = 0;

  /*
    * if the timer has expired (or assumed that if current_millis == 0, then it will be)
    * i.e. done waiting move to the next state
    */
  if(((new_millis = millis()) - current_millis) >= delta_time)  {
    current_millis = new_millis;
    neo_state = NEO_SEQ_WRITE;
  }

}

// end of SEQ_STRAT_SLOWP callbacks

/*
 * SEQ_STRAT_PONG
 * one lighted pixel moves from one end to the other and back
 * in a continuous ping-poing
 * two lines are expected in the sequence file:
 *   - first line is the starting intensity/color and time between movements
 *   - second line is the ending intensity/color
 *
 * NOTE: we're going to borrow much of the functionality and global variables
 *       from slowP ... but no flicker stuff:
 *  slowp_idx : tracks the lit pixel
 *  slowp_dir : going up or down the strand (pinging or ponging)
 */
uint16_t p_num_pixels = 0;  // will use this shortcut alot
int16_t pong_repeats = -1;
void neo_pong_start(bool clear)  {

  slowp_idx = 0;
  slowp_dir = 1;  // start by going up

  jparse_ctx_t jctx;  // for json parsing
  char jbuf[MAX_NEO_BONUS];

  pong_repeats = -1; // start with continuous

  /*
   * obtain the number of times the "single" sequence will be run
   * based on the "bonus" parameter from the json sequence file
   */
  if(strlen(neo_sequences[seq_index].bonus) > 0)  {
    ESP_LOGD(TAG, "neo_pong_start: bonus = %s", neo_sequences[seq_index].bonus);

    if(json_parse_start(&jctx, neo_sequences[seq_index].bonus, strlen(neo_sequences[seq_index].bonus)) != OS_SUCCESS)  {
      ESP_LOGE(TAG, "ERROR: Deserialization of bonus failed ... using minus one\n");
    }
    else  {
      json_obj_get_string(&jctx, "count", jbuf, sizeof(jbuf));  // used to point to place in sequence array

      if(*jbuf == '\0')  {
        ESP_LOGE(TAG, "WARNING: pong bonus has no member \"count\" ... using minus one\n");
      }
      else  {
        if(atoi(jbuf) > INT16_MAX)
          pong_repeats = INT16_MAX;
        else
          pong_repeats = atoi(jbuf);
        ESP_LOGI(TAG, "neo_pong_start: pong_repeats set to %d", pong_repeats);
      }
      json_parse_end(&jctx);  // done with json
    }
  }



  p_num_pixels = pixels_numPixels();

  /*
   * calculate delta time in mS based on the first
   * line in the json sequence file
   */
  delta_time = (neo_sequences[seq_index].point[0].ms_after_last) / p_num_pixels;

  /*
   * calculate the delta chance for each color
   * the first line in the json sequence has the max/endpoint
   * of the sequence
   *
   */
  delta_r = (neo_sequences[seq_index].point[1].red - neo_sequences[seq_index].point[0].red) / (float)(p_num_pixels-1);  // cast needed to force floating point math
  delta_g = (neo_sequences[seq_index].point[1].green - neo_sequences[seq_index].point[0].green) / (float)(p_num_pixels-1);
  delta_b = (neo_sequences[seq_index].point[1].blue - neo_sequences[seq_index].point[0].blue) / (float)(p_num_pixels-1);

  /*
   * start from the json specified starting point
   */
  slowp_r = neo_sequences[seq_index].point[0].red;
  slowp_g = neo_sequences[seq_index].point[0].green;
  slowp_b = neo_sequences[seq_index].point[0].blue;

  /*
   * clear and set the first point here
   */
  pixels_clear();
  pixels_setPixelColorRGB(slowp_idx, neo_check_range(slowp_r),
                                  neo_check_range(slowp_g),
                                  neo_check_range(slowp_b), 0);  // turn on the next one
  pixels_show();

  current_millis = millis();

  ESP_LOGD(TAG, "Starting pong: dr = %f, dg = %f, db = %f dt = %lu\n", delta_r, delta_g, delta_b, delta_time);

  neo_state = NEO_SEQ_WAIT;
}

void neo_pong_write(void) {
  
  /*
   * calculate the rgb values
   */
  if(slowp_dir > 0)  {  // currently going up
    slowp_idx++;
    if(slowp_idx < p_num_pixels)  {  // have not reached the top of the sequence
      slowp_r += delta_r;
      slowp_g += delta_g;
      slowp_b += delta_b;
    }
    else  {
      slowp_dir = -1;  // change to going down
      slowp_idx--;

      /*
       * reset to the ending point in case of rounding error
       */
      slowp_r = neo_sequences[seq_index].point[1].red;
      slowp_g = neo_sequences[seq_index].point[1].green;
      slowp_b = neo_sequences[seq_index].point[1].blue;
    }
  }

  /*
   * currently going down
   */
  else  {
    slowp_idx--;
    if(slowp_idx >= 0)  {
      slowp_r -= delta_r;
      slowp_g -= delta_g;
      slowp_b -= delta_b;
    }
    else  {
      slowp_dir = 1;  // change to going down
      slowp_idx++;

      /*
       * reset to the starting point  in case of rounding error
       */
      slowp_r = neo_sequences[seq_index].point[0].red;
      slowp_g = neo_sequences[seq_index].point[0].green;
      slowp_b = neo_sequences[seq_index].point[0].blue;

      if(pong_repeats > (int16_t)0)
        pong_repeats--;
    }
  }

  /*
   * send the next point in the sequence to the strand
   */
  pixels_clear();  // first turn them all off
  pixels_setPixelColorRGB(slowp_idx, neo_check_range(slowp_r),
                                     neo_check_range(slowp_g),
                                     neo_check_range(slowp_b), 0);  // turn on the next one
  pixels_show();   // Send the updated pixel colors to the hardware.

  if(pong_repeats == (int16_t)(-1))  // not counting keep going
    neo_state = NEO_SEQ_WAIT;
  else if (pong_repeats > 0)         // counting and still have some repeats to go
    neo_state = NEO_SEQ_WAIT;
  else                               // counting and done
    neo_state = NEO_SEQ_STOPPING;
}

// end of SEQ_STRAT_PONG

/*
 * SEQ_STRAT_RAINBOW
 * cycle a rainbow color pallette along the whole strip
 * (adapted from the Adafruit strandtest example)
 */
static uint32_t firstPixelHue = 0;
static uint8_t saturation = 255;
static uint8_t brightness = 255;
static int8_t rainbow_reps = 1;
static bool gammify = false;
static uint16_t rainbow_numpixels;

/*
 * ported Adafruit rainbow sequence code
 */

#ifdef DONTNEEDTOUSETHIS
/*!
  @brief   Change the pixel format of a previously-declared
           Adafruit_NeoPixel strip object. If format changes from one of
           the RGB variants to an RGBW variant (or RGBW to RGB), the old
           data will be deallocated and new data is cleared. Otherwise,
           the old data will remain in RAM and is not reordered to the
           new format, so it's advisable to follow up with clear().
  @param   t  Pixel type -- add together NEO_* constants defined in
              Adafruit_NeoPixel.h, for example NEO_GRB+NEO_KHZ800 for
              NeoPixels expecting an 800 KHz (vs 400 KHz) data stream
              with color bytes expressed in green, red, blue order per
              pixel.
  @note    This function is deprecated, here only for old projects that
           may still be calling it. New projects should instead use the
           'new' keyword with the first constructor syntax
           (length, pin, type).
*/
typedef uint16_t neoPixelType; ///< 3rd arg to Adafruit_NeoPixel constructor
neoPixelType wOffset, rOffset, gOffset, bOffset;
void Adafruit_NeoPixel_updateType(neoPixelType t) {
  bool oldThreeBytesPerPixel = (wOffset == rOffset); // false if RGBW

  wOffset = (t >> 6) & 0b11; // See notes in header file
  rOffset = (t >> 4) & 0b11; // regarding R/G/B/W offsets
  gOffset = (t >> 2) & 0b11;
  bOffset = t & 0b11;
#if defined(NEO_KHZ400)
  is800KHz = (t < 256); // 400 KHz flag is 1<<8
#endif

  // If bytes-per-pixel has changed (and pixel data was previously
  // allocated), re-allocate to new size. Will clear any data.
  if (pixels) {
    bool newThreeBytesPerPixel = (wOffset == rOffset);
    if (newThreeBytesPerPixel != oldThreeBytesPerPixel)
      updateLength(numLEDs);
  }
}
#endif


/*!
  @brief   Set a pixel's color using a 32-bit 'packed' RGB or RGBW value.
  @param   n  Pixel index, starting from 0.
  @param   c  32-bit color value. Most significant byte is white (for RGBW
              pixels) or ignored (for RGB pixels), next is red, then green,
              and least significant byte is blue.
*/
void Adafruit_NeoPixel_setPixelColor(uint16_t n, uint32_t c) {
  uint8_t r = (uint8_t)(c >> 16), g = (uint8_t)(c >> 8), b = (uint8_t)c;
  if (n < rainbow_numpixels) {
    if (brightness) { // See notes in setBrightness()
      r = (r * brightness) >> 8;
      g = (g * brightness) >> 8;
      b = (b * brightness) >> 8;
    }
    pixels_setPixelColorRGB(n, r, g, b, 0);
  }
}

/*!
  @brief   Convert hue, saturation and value into a packed 32-bit RGB color
           that can be passed to setPixelColor() or other RGB-compatible
           functions.
  @param   hue  An unsigned 16-bit value, 0 to 65535, representing one full
                loop of the color wheel, which allows 16-bit hues to "roll
                over" while still doing the expected thing (and allowing
                more precision than the wheel() function that was common to
                prior NeoPixel examples).
  @param   sat  Saturation, 8-bit value, 0 (min or pure grayscale) to 255
                (max or pure hue). Default of 255 if unspecified.
  @param   val  Value (brightness), 8-bit value, 0 (min / black / off) to
                255 (max or full brightness). Default of 255 if unspecified.
  @return  Packed 32-bit RGB with the most significant byte set to 0 -- the
           white element of WRGB pixels is NOT utilized. Result is linearly
           but not perceptually correct, so you may want to pass the result
           through the gamma32() function (or your own gamma-correction
           operation) else colors may appear washed out. This is not done
           automatically by this function because coders may desire a more
           refined gamma-correction function than the simplified
           one-size-fits-all operation of gamma32(). Diffusing the LEDs also
           really seems to help when using low-saturation colors.
*/
uint32_t Adafruit_ColorHSV(uint16_t hue, uint8_t sat, uint8_t val) {

  uint8_t r, g, b;

  // Remap 0-65535 to 0-1529. Pure red is CENTERED on the 64K rollover;
  // 0 is not the start of pure red, but the midpoint...a few values above
  // zero and a few below 65536 all yield pure red (similarly, 32768 is the
  // midpoint, not start, of pure cyan). The 8-bit RGB hexcone (256 values
  // each for red, green, blue) really only allows for 1530 distinct hues
  // (not 1536, more on that below), but the full unsigned 16-bit type was
  // chosen for hue so that one's code can easily handle a contiguous color
  // wheel by allowing hue to roll over in either direction.
  hue = (hue * 1530L + 32768) / 65536;
  // Because red is centered on the rollover point (the +32768 above,
  // essentially a fixed-point +0.5), the above actually yields 0 to 1530,
  // where 0 and 1530 would yield the same thing. Rather than apply a
  // costly modulo operator, 1530 is handled as a special case below.

  // So you'd think that the color "hexcone" (the thing that ramps from
  // pure red, to pure yellow, to pure green and so forth back to red,
  // yielding six slices), and with each color component having 256
  // possible values (0-255), might have 1536 possible items (6*256),
  // but in reality there's 1530. This is because the last element in
  // each 256-element slice is equal to the first element of the next
  // slice, and keeping those in there this would create small
  // discontinuities in the color wheel. So the last element of each
  // slice is dropped...we regard only elements 0-254, with item 255
  // being picked up as element 0 of the next slice. Like this:
  // Red to not-quite-pure-yellow is:        255,   0, 0 to 255, 254,   0
  // Pure yellow to not-quite-pure-green is: 255, 255, 0 to   1, 255,   0
  // Pure green to not-quite-pure-cyan is:     0, 255, 0 to   0, 255, 254
  // and so forth. Hence, 1530 distinct hues (0 to 1529), and hence why
  // the constants below are not the multiples of 256 you might expect.

  // Convert hue to R,G,B (nested ifs faster than divide+mod+switch):
  if (hue < 510) { // Red to Green-1
    b = 0;
    if (hue < 255) { //   Red to Yellow-1
      r = 255;
      g = hue;       //     g = 0 to 254
    } else {         //   Yellow to Green-1
      r = 510 - hue; //     r = 255 to 1
      g = 255;
    }
  } else if (hue < 1020) { // Green to Blue-1
    r = 0;
    if (hue < 765) { //   Green to Cyan-1
      g = 255;
      b = hue - 510;  //     b = 0 to 254
    } else {          //   Cyan to Blue-1
      g = 1020 - hue; //     g = 255 to 1
      b = 255;
    }
  } else if (hue < 1530) { // Blue to Red-1
    g = 0;
    if (hue < 1275) { //   Blue to Magenta-1
      r = hue - 1020; //     r = 0 to 254
      b = 255;
    } else { //   Magenta to Red-1
      r = 255;
      b = 1530 - hue; //     b = 255 to 1
    }
  } else { // Last 0.5 Red (quicker than % operator)
    r = 255;
    g = b = 0;
  }

  // Apply saturation and value to R,G,B, pack into 32-bit result:
  uint32_t v1 = 1 + val;  // 1 to 256; allows >>8 instead of /255
  uint16_t s1 = 1 + sat;  // 1 to 256; same reason
  uint8_t s2 = 255 - sat; // 255 to 0
  return ((((((r * s1) >> 8) + s2) * v1) & 0xff00) << 8) |
         (((((g * s1) >> 8) + s2) * v1) & 0xff00) |
         (((((b * s1) >> 8) + s2) * v1) >> 8);
}

/*!
  @brief   Fill NeoPixel strip with one or more cycles of hues.
           Everyone loves the rainbow swirl so much, now it's canon!
  @param   first_hue   Hue of first pixel, 0-65535, representing one full
                       cycle of the color wheel. Each subsequent pixel will
                       be offset to complete one or more cycles over the
                       length of the strip.
  @param   reps        Number of cycles of the color wheel over the length
                       of the strip. Default is 1. Negative values can be
                       used to reverse the hue order.
  @param   saturation  Saturation (optional), 0-255 = gray to pure hue,
                       default = 255.
  @param   brightness  Brightness/value (optional), 0-255 = off to max,
                       default = 255. This is distinct and in combination
                       with any configured global strip brightness.
  @param   gammify     If true (default), apply gamma correction to colors
                       for better appearance.
*/
void Adafruit_NeoPixel_rainbow(uint16_t first_hue, int8_t reps,
  uint8_t saturation, uint8_t brightness, bool gammify) {
  for (uint16_t i = 0; i < rainbow_numpixels; i++) {
    uint16_t hue = first_hue + (i * reps * 65536) / rainbow_numpixels;
    uint32_t color = Adafruit_ColorHSV(hue, saturation, brightness);
//    if (gammify) color = gamma32(color);
    Adafruit_NeoPixel_setPixelColor(i, color);
  }
}


// end of Adafruit code
#define NEO_RAINBOW_BRIGHTNESS 32  // Adafruit defined brightness
#define NEO_RAINBOW_SATURATION 128 // Adafruit defined saturation
#define NEO_RAINBOW_INTERVAL   16  // mS between updates (~60 Hz updates)
#define NEO_RAINBOW_REPS       2   // number of replicants of color wheel across strand

void neo_rainbow_start(bool clear)  {

  uint32_t firstPixelHue = NEO_RAINBOW_INTERVAL;
  uint8_t saturation = NEO_RAINBOW_SATURATION;
  uint8_t brightness = NEO_RAINBOW_BRIGHTNESS;
  int8_t reps = NEO_RAINBOW_REPS;
  bool gammify = false;
  rainbow_numpixels = pixels_numPixels();

  ESP_LOGI(TAG, "Starting rainbow for %u pixels", rainbow_numpixels);
  pixels_clear();
  pixels_show();
  current_millis = millis();
  neo_state = NEO_SEQ_WRITE;
}

/*
 * wait a fixed 10mS
 */
void neo_rainbow_wait(void)  {
  uint64_t new_millis = 0;

  /*
    * if the timer has expired (or assumed that if current_millis == 0, then it will be)
    * i.e. done waiting move to the next state
    */
  if(((new_millis = millis()) - current_millis) >= 10)  {
    current_millis = new_millis;
    neo_state = NEO_SEQ_WRITE;
  }
}

/*
 * advance and write a pixel
 */
void neo_rainbow_write(void) {
  Adafruit_NeoPixel_rainbow(firstPixelHue, rainbow_reps, saturation, brightness, gammify);
  pixels_show();

  firstPixelHue += 256;

  if(firstPixelHue >= 5*65536)
    firstPixelHue = 0;

  neo_state = NEO_SEQ_WAIT;

}

void neo_rainbow_stopping(void)  {
  pixels_clear(); // Set all pixel colors to 'off'
  pixels_show();   // Send the updated pixel colors to the hardware.

  seq_index = -1; // so it doesn't match

  neo_state = NEO_SEQ_STOPPED;
}

// end of SEQ_STRAT_RAINBOW callbacks

/*
 * SEQ_STRAT_BWISE
 * read a file containing bit pattern, addressing of individual
 * pixels in the strand using a single R, G, B value set, and 
 * play it out
 */
uint8_t bw_r, bw_g, bw_b, bw_w;  // for the on color
int8_t bw_idepth = 1;  // number of n pixel chunks ... extracted on start
int32_t bw_ms_after_last;  // time interval between points; read dynamically

/*
 * int32_t neo_bitwise_write_point(int32_t t, bool clear, bool show)
 *
 *
 * extract the on/off desired bitwise state of the array
 * and write r, g, b to the array
 * 
 * use a mask to extract the bitwise on/off pixel/color value
 * and write the global bw_r, g, b as indicated.
 * 
 * this function will set pixel values up to the physical number
 * of pixels defined in pmonconfig (i.e. eeprom) only.  Any beyond
 * that in the binary data are ignored.
 * 
 * pixels_show() is *optionally* called to play out the pixels.
 * 
 * 
 * arguments:
 *   int32_t *t : time interval from binary parsing
 *   bool clear : clear the pixels before updating
 *   bool show: send the pixels to the strand hardware
 * 
 * globals:
 *   seq_index : index of sequence being played out
 *   neo_sequences[seq_index].alt_points : pointer to binary/bitwise data
 *   current_index : current point in sequence
 * 
 * return:
 *   esp_err_t err : error code returned from attempt to write pixel values
 * 
 *    (NOTE: this is a bitwise point representing all pixels)
 */
esp_err_t neo_bitwise_write_point(int32_t *t, bool clear, bool show) {
  esp_err_t err = ESP_OK;
  esp_err_t nerr = ESP_OK;
  uint32_t mask = 0x00000001;  // 32 bit mask to interogate data - first pixel
  uint32_t p_num_pixels = 0;  // number of pixels set in config eeprom
  uint32_t pixel_cnt = 0;  // how many pixels have been set
  seq_bin_t *cpointrow;  // shorthand pointer to start of 32 bit row
  int8_t d = 0;  // depth/row counter
  uint8_t r, g, b, w;  // appropriately sized color values

  if(clear == true)  pixels_clear(); // Set all pixel colors to 'off'

  cpointrow = (seq_bin_t *)neo_sequences[seq_index].alt_points;
  cpointrow += (current_index * bw_idepth);  // increments by the size of cpointrow
  ESP_LOGD(TAG, "cpointrow - 0x%x", (unsigned int)cpointrow);

  p_num_pixels = pixels_numPixels();
  ESP_LOGD(TAG, "Updating %lu pixels with mask", p_num_pixels);
  pixel_cnt = 0;  // being overly obvious
  for(d = 0; d < bw_idepth; d++)  {  // rows (i.e. pixel depth )
    ESP_LOGD(TAG, "Offset from data %d", cpointrow->o);
    mask = 0x00000001;
    for(int8_t p = 0; p < PIXELS_PER_JSON_ROW; p++)  {  // pixels
      if(pixel_cnt < p_num_pixels)  {
        r = ((cpointrow->r  & mask) ? bw_r : 0);
        g = ((cpointrow->g  & mask) ? bw_g : 0);
        b = ((cpointrow->b  & mask) ? bw_b : 0);
        w = ((cpointrow->w  & mask) ? bw_w : 0);
        if((nerr = pixels_setPixelColorRGB(pixel_cnt, r, g, b, w)) != ESP_OK)
          err = nerr;  // remember any error codes returned, but don't abort the attempt
        *t = cpointrow->d;  // delay, only the last one counts
      }
      pixel_cnt++;
      mask = mask << 1;  // next pixel in this color's mask
    }
    cpointrow++;
  }
  if(show == true)
    pixels_show();
  return(err);  // pointing to time interval
}


/*
 * int32_t neo_bitwise_write_servo(bool clear, bool show)
 *
 * extract the on/off desired bitwise state of the array
 * and write r, g, b to the array
 * 
 * use a mask to extract the bitwise on/off pixel/color value
 * and write the global bw_r, g, b as indicated.
 * 
 * this function will set pixel values up to the physical number
 * of pixels defined in pmonconfig (i.e. eeprom) only.  Any beyond
 * that in the binary data are ignored.
 * 
 * pixels_show() is *optionally* called to play out the pixels.
 * 
 * 
 * arguments:
 *   bool clear : clear the pixels before updating
 *   bool show: send the pixels to the strand hardware
 * 
 * globals:
 *   seq_index : index of sequence being played out
 *   neo_sequences[seq_index].alt_points : pointer to binary/bitwise data
 *   current_index : current point in sequence
 * 
 * return:
 *   int32_t t : time interval from point data
 *               (can be -1 to indicate a point data terminator)
 * 
 *    (NOTE: this is a bitwise point representing all pixels)
 */
void neo_bitwise_write_servo(void) {
  uint32_t mask = 0x00000001;  // 32 bit mask to interogate data - first servo
  uint32_t p_num_servos = servo_get_numservos();  // number of pixels set in config eeprom
  uint8_t servo_cnt = 0;  // how many pixels have been set
  seq_bin_t *cpointrow;  // shorthand pointer to start of 32 bit row
  int8_t d = 0;  // depth/row counter
  int32_t aangle = 0; // achieved absolute angle

  cpointrow = (seq_bin_t *)neo_sequences[seq_index].alt_points;
  cpointrow += (current_index * bw_idepth);  // increments by the size of cpointrow
  ESP_LOGD(TAG, "cpointrow - 0x%x", (unsigned int)cpointrow);

  servo_cnt = 0;  // being overly obvious
  for(d = 0; d < bw_idepth; d++)  {  // rows (i.e. pixel depth )
    ESP_LOGD(TAG, "Offset from data %d", cpointrow->o);
    mask = 0x00000001;
    for(int8_t p = 0; p < SERVOS_PER_JSON_ROW; p++)  {  // servos
      if(servo_cnt < p_num_servos)  {
        if((cpointrow->s  & mask) != (uint32_t)0)
          servo_move_real_pre(servo_cnt, cpointrow->a, false, &aangle);
      }
      servo_cnt++;
      mask = mask << 1;  // next pixel in this color's mask
    }
    cpointrow++;
  }
}

/*
 * parse the json bonus to know the on rgb, set the timer
 * and write the first point.
 */
void neo_bitwise_start(bool clear)  {
  jparse_ctx_t bjctx;  // for locally parsing the bonus string
  int bval = 0;

  /*
   * set a default color in case the json parsing doesn't work
   * red to indicate issue
   */
  bw_r = 64;
  bw_g = 0;
  bw_b = 0;
  bw_w = 0;

  /*
   * parse the depth and color to be assigned to on pixels from the passed
   * bonus string
   * NOTE: don't really need to parse the bonus string here ... leave for reference.
   */
  if(json_parse_start(&bjctx, neo_sequences[seq_index].bonus, strlen(neo_sequences[seq_index].bonus)) != OS_SUCCESS)  {
    ESP_LOGE(TAG, "ERROR: bitwise_start: Deserialization of bonus\n");

  }
  else  {
    json_obj_get_int(&bjctx, "depth", &bval);  // used to point to place in sequence array
      bw_idepth = bval;
    json_obj_get_object(&bjctx, "brightness");  // reserialized for later use
    json_obj_get_int(&bjctx, "r", &bval);
    bw_r = bval;
    json_obj_get_int(&bjctx, "g", &bval);
    bw_g = bval;
    json_obj_get_int(&bjctx, "b", &bval);
    bw_b = bval;
    json_obj_get_int(&bjctx, "w", &bval);
    bw_w = bval;
    json_obj_leave_object(&bjctx);  // leave brightness
    json_parse_end(&bjctx);  // leave bonus
  }
  ESP_LOGI(TAG, "Using on values of %u %u %u %u", bw_r, bw_g, bw_b, bw_w);

  current_millis = millis();  // set the timer

  // write the first pixel and show it
  neo_bitwise_write_point(&bw_ms_after_last, true, true);
  ESP_LOGD(TAG, "Using time interval of %ld", bw_ms_after_last);
  neo_state = NEO_SEQ_WAIT;

}

void neo_bitwise_write(void) {
  /*
   * write the pixel, but don't show yet
   */
  neo_bitwise_write_point(&bw_ms_after_last, false, false);
  if(bw_ms_after_last < 0)  {  // list terminator from the last write: don't display
    current_index = 0;
    ESP_LOGD(TAG, "Back to point %ld", current_index);
    neo_state = NEO_SEQ_WRITE;  // loop back around and write the top of list
  }
  else  {
    pixels_show();
    neo_bitwise_write_servo();
    neo_state = NEO_SEQ_WAIT;
  }

  ESP_LOGD(TAG, "neo_bitwise_write() using time interval of %ld", bw_ms_after_last);
}

void neo_bitwise_wait(void)  {
  uint64_t new_millis = 0;

  /*
    * if the timer has expired (or assumed that if current_millis == 0, then it will be)
    * i.e. done waiting move to the next state
    */
  if(((new_millis = millis()) - current_millis) >= bw_ms_after_last)  {
    ESP_LOGD(TAG, "... time's up !");
    current_millis = new_millis;
    current_index++;
    neo_state = NEO_SEQ_WRITE;
  }

}

void neo_bitwise_stopping(void)  {
   /*
    * move to top to avoid potential collision with late
    * coming meo_cycle_next()
    */
  neo_state = NEO_SEQ_STOPPED;

  pixels_clear(); // Set all pixel colors to 'off'
  pixels_show();   // Send the updated pixel colors to the hardware.

  if(neo_sequences[seq_index].alt_points != NULL)  {
    free(neo_sequences[seq_index].alt_points);
    ESP_LOGI(TAG, "binary point data free()'ed");
  }

  current_index = 0;  // housekeeping
  seq_index = -1; // so it doesn't match
  current_strategy = SEQ_STRAT_POINTS;  // housekeeping

}
// end of SEQ_STRAT_BWISE

// start of SEQ_STRAT_SCRIPT

/*
 * had off execution of the script to the script engine
 * and stop its execution here
 */
void neo_script_start(bool clear)  {
  neo_state = NEO_SEQ_STOPPING;
}

void neo_script_stopping(void)  {

  /*
   * stop anything that was going on here in the pixel engine.
   *
   * (move to top to avoid potential collision with late
   * coming neo_cycle_next())
   */
  neo_state = NEO_SEQ_STOPPED;

  pixels_clear(); // Set all pixel colors to 'off'
  pixels_show();   // Send the updated pixel colors to the hardware.

  /*
   * set up the command to tell the script engine to get started

   * note: do not free neo_sequences[seq_index].alt_points 
   * in this case since the script engine will use it
   * 
   * note: do this before resetting the sequence indexes
   */
  script_mutex_data_t script_cmd;
  script_cmd.cmd_type = NEO_CMD_SCRIPT_START;
  script_cmd.new_data = true;
  script_cmd.steps = (neo_script_step_t *)(neo_sequences[seq_index].alt_points);

  /*
   * housekeeping (prepping for the first sequence that the script
   * engine will run)
   */
  current_index = 0;  // housekeeping
  seq_index = -1; // so it doesn't match
  current_strategy = SEQ_STRAT_POINTS;  // housekeeping

  /*
   * inform the script engine to get started
   */
  if(neo_script_send_msg(script_cmd) == pdTRUE)
    ESP_LOGI(TAG, "script command (%d) sent successfully", script_cmd.cmd_type);
  else
    ESP_LOGE(TAG, "error sending script command (%d)", script_cmd.cmd_type);
}

// end of SEQ_STRAT_SCRIPT


/*
 * function calls by filetype
 * one of these functions are called to process the char *buf 
 * containing the balance of the file after the first line header
 * determines the filetype.
 */

neo_ftype_t neo_file_procs[] = {
  {"OG", neo_proc_OG, data_valid_OG},
  {"BIN_BW", neo_proc_BIN_BBW, data_valid_BIN_BBW},
  {"SCRIPT", neo_proc_SCRIPT, data_valid_SCRIPT},
  {"\0", NULL, NULL}
};

/*
 * function calls by strategy for each state in the playback machine
 * TODO: delete the 'x' before the labels after implementing a strategy
 */
seq_callbacks_t seq_callbacks[NEO_SEQ_STRATEGIES] = {
//  strategy              label             prep            start                wait              write                stopping             stopped
  { SEQ_STRAT_POINTS,    "points",    parse_pts_OG,     neo_points_start,   neo_points_wait,   neo_points_write,    neo_points_stopping,     noop},
  { SEQ_STRAT_SINGLE,    "single",    parse_pts_OG,     neo_single_start,   neo_points_wait,   neo_single_write,    neo_points_stopping,     noop},
  { SEQ_STRAT_CHASE,     "xchase",    parse_pts_OG,      start_noop,           noop,               noop,                 noop,               noop},
  { SEQ_STRAT_PONG,      "pong",      parse_pts_OG,     neo_pong_start,     neo_slowp_wait,    neo_pong_write,      neo_points_stopping,     noop},
  { SEQ_STRAT_RAINBOW,   "rainbow",   parse_pts_OG,    neo_rainbow_start, neo_rainbow_wait,  neo_rainbow_write,     neo_rainbow_stopping,     noop},
  { SEQ_STRAT_SLOWP,     "slowp",     parse_pts_OG,     neo_slowp_start,    neo_slowp_wait,    neo_slowp_write,     neo_points_stopping,     noop},
  { SEQ_STRAT_BWISE,     "bitwise",   parse_pts_BW,       start_noop,            noop,               noop,          neo_bitwise_stopping,    noop},
  { SEQ_STRAT_BBWISE,    "bbitwise",  parse_pts_BBW,    neo_bitwise_start,  neo_bitwise_wait,  neo_bitwise_write,     neo_bitwise_stopping,  noop},
  { SEQ_STRAT_SCRIPT,    "script",    parse_pts_SCRIPT,  neo_script_start,       noop,              noop,             neo_script_stopping,   noop},
};




/*
 * expose a method to set the strategy from the "main"
 * look through the labels for a match with the argument
 * and set the global 
 */
seq_strategy_t neo_set_strategy(const char *sstrategy)  {
  seq_strategy_t ret = SEQ_STRAT_UNDEFINED;

  for(int8_t i=0; i < NEO_SEQ_STRATEGIES; i++)  {
    if(strcmp(sstrategy, seq_callbacks[i].label) == 0)  {
      ret = seq_callbacks[i].strategy;
    }
  }
  return(ret);
}

/*
 * check if the specified time since last change has occured
 * and update the strand if so.
 */
void neo_cycle_next(void)  {

  switch(neo_state)  {

    case NEO_SEQ_STOPPED:
      seq_callbacks[current_strategy].stopped();
      break;

    case NEO_SEQ_STOPPING:
      seq_callbacks[current_strategy].stopping();
      neo_script_progress_msg(pending_script_cmd);  //signal the script engine if it's running
      pending_script_cmd = NEO_SCRIPT_UNDEFINED;  // housekeeping for later
      break;

    case NEO_SEQ_START:
      seq_callbacks[current_strategy].start(true);  // clear the strand and write the first value
      break;
    
    case NEO_SEQ_WAIT:
      seq_callbacks[current_strategy].wait();
      break;
    
    case NEO_SEQ_WRITE:
      seq_callbacks[current_strategy].write();
      break;

    default:
      ESP_LOGD(TAG, "Invalid State");  //thread safety test
      break;
  }
}

/*
 * stop the sequence i.e. turn off neopixel strand
 */
void neo_cycle_stop(void)  {
  neo_state = NEO_SEQ_STOPPING;
  //seq_index = -1;  // so it doesn't match ... move to _stopping()
}

/*
 * was a new sequence posted from the controlling process
 * NOTE: neo_set_sequence() determines if this is different
 * than the currently running sequence.
 */
int8_t neo_new_sequence(void)  {
  int8_t neoerr = NEO_SUCCESS;
  neo_mutex_data_t l_neo;  // local copy so as not to hold the mutex

  l_neo.new_data = false;  // was a new request made

  /*
   * try to grab the semaphore to check if a new sequence was requesed
   * (just poll, don't wait ... you'll get it the next time around)
   */
  if(xSemaphoreTake(xneoMutex, 0) == pdTRUE)  {
    if(neo_mutex_data.new_data == true)  {
      memcpy(&l_neo, &neo_mutex_data, sizeof(neo_mutex_data_t));
      neo_mutex_data.new_data = false;
      // only uncomment the following msg statement if you slow down the update rate
      //ESP_LOGI(TAG, "neo_new_sequence: new data received and successful xneoMutex take");
    }
    xSemaphoreGive(xneoMutex);  // be done with it ASAP
  }

  /*
   * if a new request was sent
   */
  if(l_neo.new_data == true)  {
    pending_script_cmd = NEO_CMD_SCRIPT_UNDEFINED;

    /*
    * process the button that was pressed based on the seq string
    */
    if(l_neo.sequence[0] != '\0')  {  // the request has a "non-zero" sequence
      ESP_LOGI(TAG, "neo_new_sequence:  %s", l_neo.sequence);

      /*
       * determine what kind of request was sent
       */
      if(strcmp(l_neo.sequence, "none") == 0)  {  // primarily used to indicate default/startup of "none"
        neoerr = NEO_NEW_SUCCESS;
      }
      else if(strcmp(l_neo.sequence, "STOP") == 0)  {  // STOP button pressed
        /*
         * stop a script (subsequent logic tests to see if its running)
         */
        neo_script_progress_msg(NEO_CMD_SCRIPT_STOP_REQ);
        neo_script_verify_stop();  // blocks up to SCRIPT_STOP_PER_INTERVAL * SCRIPT_STOP_INTERVALS mS

        /*
         * if a sequence is running, stop it
         */
        if((neo_state != NEO_SEQ_STOPPED) && (neo_state != NEO_SEQ_STOPPING))  {  // sequence is running
          neoerr = NEO_NEW_SUCCESS;
          neo_cycle_stop();
        }
        else
          neoerr = NEO_OLD_SUCCESS;  // no change
      }

      else if(strcmp(l_neo.sequence, "NEXT") == 0)  {  // NEXT button pressed ... ignore if no script running
        pending_script_cmd = NEO_CMD_SCRIPT_STEP_NEXT;  // script running tested in sending routine
        if(neo_script_is_running(0) == true)  {
         /*
          * if a sequence is running, stop it
          * sequences stop function may notify the script engine if running
          */
          if((neo_state != NEO_SEQ_STOPPED) && (neo_state != NEO_SEQ_STOPPING))  {  // sequence is running
            neoerr = NEO_NEW_SUCCESS;
            neo_cycle_stop();
          }
          else
            neoerr = NEO_OLD_SUCCESS;  // no change
        }
        else
          neoerr = NEO_OLD_SUCCESS;  // no change
      }

      /*
       * return to the previous step in the script
       * STUB FOR NOW
       */
      else if(strcmp(l_neo.sequence, "PREVIOUS") == 0)  {
        pending_script_cmd = NEO_CMD_SCRIPT_STEP_PREV;  // script running tested in sending routine
        if(neo_script_is_running(0) == true)  {
         /*
          * if a sequence is running, stop it
          * sequences stop function may notify the script engine if running
          */
          if((neo_state != NEO_SEQ_STOPPED) && (neo_state != NEO_SEQ_STOPPING))  {  // sequence is running
            neoerr = NEO_NEW_SUCCESS;
            neo_cycle_stop();
          }
          else
            neoerr = NEO_OLD_SUCCESS;  // no change
        }
        else
          neoerr = NEO_OLD_SUCCESS;  // no change
      }

      /*
        * if not STOP, see if it was a USER defined sequence
        * if so, load the file and set the sequence and strategy
        */
      else if((neo_is_user(l_neo.sequence)) == NEO_SUCCESS)  {
        /*
         * if this is a sequence button press from the UI,
         * stop the script if running.  wait for it to stop.
         * note: if a script is not running, the command will be
         * ignored and the verify will return instantly.
         */
        if(l_neo.resp_reqd == true)  {
          neo_script_progress_msg(NEO_CMD_SCRIPT_STOP_REQ);
          neo_script_verify_stop();  // blocks up to SCRIPT_STOP_PER_INTERVAL * SCRIPT_STOP_INTERVALS mS
        }

        if((neoerr = neo_load_sequence(l_neo.file)) < NEO_SUCCESS)
          ESP_LOGE(TAG, "Error loading sequence file after proper detection");
      }

      /*
        * if not STOP or USER-x, then attempt to set the sequence,
        * assuming that it's a pre-defined button.
        * strategies are hardcoded for built in sequences.
        */
      else  {
        /*
         * if this is a sequence button press from the UI,
         * stop the script if running.  wait for it to stop.
         * note: if a script is not running, the command will be
         * ignored and the verify will return instantly.
         */
        if(l_neo.resp_reqd == true)  {
          neo_script_progress_msg(NEO_CMD_SCRIPT_STOP_REQ);
          neo_script_verify_stop();  // blocks up to SCRIPT_STOP_PER_INTERVAL * SCRIPT_STOP_INTERVALS mS
        }

        if((neoerr = neo_set_sequence(l_neo.sequence, "")) < NEO_SUCCESS)
          ESP_LOGI(TAG, "ERROR: Error setting sequence after proper detection");
      }
    }
    else  {
      neoerr = NEO_NOPLACE;
      ESP_LOGI(TAG, "ERROR: \"sequence\" not found in json data");
    }
  }
  if((neoerr >= NEO_SUCCESS) && (l_neo.resp_reqd == false))
    neoerr = NEO_NOR_SUCCESS;
  return(neoerr);
}

